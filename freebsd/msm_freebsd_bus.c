/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 James Kane
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * The ACPI GPU device (QCOM0636): powers the GPU's always-on side, claims
 * its SMMU, and has msm_freebsd.c create the platform devices msm's Linux
 * drivers attach to.  Also the interrupts of those devices.
 *
 * The GPU's CX power domain stays up while the driver is attached; the GMU
 * firmware powers the GPU core itself.
 *
 * This file uses bus resources, so it is built without the Linux headers.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/sysctl.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <dev/acpica/acpivar.h>

#include <dev/qcom_gpucc/qcom_gpucc.h>
#include <dev/qcom_smmu/qcom_smmu.h>

#include "msm_freebsd_bus.h"

SYSCTL_NODE(_hw, OID_AUTO, msm, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "msm DRM driver parameters");

#define	MSM_FBSD_EXTRA_RID	16		/* for interrupts not in ACPI */

static struct msm_fbsd_softc {
	device_t		dev;
	struct resource		*gmu_res;
	int			gmu_rid;
	struct qcom_gpucc	*gpucc;
	struct qcom_smmu	*smmu;
	int			next_rid;
	int			soc;		/* msm_fbsd_linux_soc() index */
	uint64_t		gpucc_pa;
} *msm_fbsd_sc;

/* Interrupts */

/*
 * The FreeBSD interrupt number of a GPU interrupt, which the glue gives its
 * platform device for LinuxKPI's request_irq().  ACPI lists most, at
 * acpi_rid; the others are added by GSIV, which maps them.
 */
int
msm_fbsd_bus_irq(int gsiv, int acpi_rid)
{
	struct msm_fbsd_softc *sc = msm_fbsd_sc;
	rman_res_t start, count;
	int rid;

	if (sc == NULL)
		return (-ENXIO);
	if (acpi_rid >= 0)
		rid = acpi_rid;
	else {
		rid = sc->next_rid++;
		if (bus_set_resource(sc->dev, SYS_RES_IRQ, rid, gsiv, 1) != 0)
			return (-ENXIO);
	}
	if (bus_get_resource(sc->dev, SYS_RES_IRQ, rid, &start, &count) != 0)
		return (-ENXIO);
	return ((int)start);
}

/* ACPI GPU device */

static char *msm_fbsd_acpi_ids[] = { "QCOM0636", NULL };

/* The index of the SoC whose power controller \\_SB.PEP0 is, or -1. */
static int
msm_fbsd_find_soc(uint64_t *gpucc_pa)
{
	ACPI_HANDLE pep;
	const char *hid;
	int i;

	if (ACPI_FAILURE(AcpiGetHandle(NULL, "\\_SB.PEP0", &pep)))
		return (-1);
	for (i = 0; msm_fbsd_linux_soc(i, &hid, gpucc_pa); i++)
		if (acpi_MatchHid(pep, hid) == ACPI_MATCHHID_HID)
			return (i);
	return (-1);
}

static int
msm_fbsd_probe(device_t dev)
{
	uint64_t gpucc_pa;
	int rv;

	rv = ACPI_ID_PROBE(device_get_parent(dev), dev, msm_fbsd_acpi_ids,
	    NULL);
	if (rv > 0 || msm_fbsd_find_soc(&gpucc_pa) < 0)
		return (ENXIO);
	device_set_desc(dev, "Qualcomm Adreno GPU");
	return (rv);
}

static void
msm_fbsd_teardown(struct msm_fbsd_softc *sc)
{
	int rid;

	/* Forget the interrupts added for want of ACPI entries. */
	for (rid = MSM_FBSD_EXTRA_RID; rid < sc->next_rid; rid++)
		bus_delete_resource(sc->dev, SYS_RES_IRQ, rid);
	sc->next_rid = MSM_FBSD_EXTRA_RID;
	qcom_smmu_release(sc->smmu);
	sc->smmu = NULL;
	qcom_gpucc_destroy(sc->gpucc);
	sc->gpucc = NULL;
	if (sc->gmu_res != NULL)
		bus_release_resource(sc->dev, SYS_RES_MEMORY, sc->gmu_rid,
		    sc->gmu_res);
	sc->gmu_res = NULL;
}

static int
msm_fbsd_attach(device_t dev)
{
	struct msm_fbsd_softc *sc = device_get_softc(dev);
	rman_res_t start, count;
	u_int sids[32], nsids;
	int error, rid;

	if (msm_fbsd_sc != NULL)
		return (ENXIO);
	sc->dev = dev;
	sc->next_rid = MSM_FBSD_EXTRA_RID;
	sc->soc = msm_fbsd_find_soc(&sc->gpucc_pa);

	/* The GPU CC is inside the GMU window, which ACPI lists instead. */
	for (rid = 0; bus_get_resource(dev, SYS_RES_MEMORY, rid, &start,
	    &count) == 0; rid++)
		if (sc->gpucc_pa >= start && sc->gpucc_pa < start + count)
			break;
	sc->gmu_rid = rid;
	sc->gmu_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->gmu_rid,
	    RF_ACTIVE);
	if (sc->gmu_res == NULL) {
		device_printf(dev, "cannot map the GPU clock controller\n");
		return (ENXIO);
	}
	sc->gpucc = qcom_gpucc_create(dev, sc->gmu_res,
	    sc->gpucc_pa - rman_get_start(sc->gmu_res));
	if (sc->gpucc == NULL) {
		error = ENXIO;
		goto fail;
	}
	if ((error = qcom_gpucc_cx_enable(sc->gpucc)) != 0) {
		device_printf(dev, "cannot power the GPU: %d\n", error);
		goto fail;
	}
	nsids = nitems(sids);
	if ((error = qcom_smmu_claim(dev, &sc->smmu, sids, &nsids)) != 0) {
		device_printf(dev, "cannot claim the GPU's SMMU: %d\n", error);
		goto fail;
	}

	msm_fbsd_sc = sc;
	error = -msm_fbsd_linux_attach(dev, sc->soc, sc->smmu);
	if (error != 0) {
		msm_fbsd_linux_detach();
		msm_fbsd_sc = NULL;
		goto fail;
	}
	return (0);

fail:
	msm_fbsd_teardown(sc);
	return (error);
}

static int
msm_fbsd_detach(device_t dev)
{
	struct msm_fbsd_softc *sc = device_get_softc(dev);

	msm_fbsd_linux_detach();
	msm_fbsd_sc = NULL;
	msm_fbsd_teardown(sc);
	return (0);
}

static device_method_t msm_fbsd_methods[] = {
	DEVMETHOD(device_probe,		msm_fbsd_probe),
	DEVMETHOD(device_attach,	msm_fbsd_attach),
	DEVMETHOD(device_detach,	msm_fbsd_detach),
	DEVMETHOD_END
};

static driver_t msm_fbsd_driver = {
	"msm",
	msm_fbsd_methods,
	sizeof(struct msm_fbsd_softc),
};

DRIVER_MODULE(msm, acpi, msm_fbsd_driver, 0, 0);
ACPI_PNP_INFO(msm_fbsd_acpi_ids);
MODULE_DEPEND(msm, acpi, 1, 1, 1);
MODULE_DEPEND(msm, drmn, 2, 2, 2);
MODULE_DEPEND(msm, dmabuf, 1, 1, 1);
MODULE_DEPEND(msm, linuxkpi, 1, 1, 1);
MODULE_DEPEND(msm, linuxkpi_video, 1, 1, 1);
MODULE_DEPEND(msm, qcom_gpucc, 1, 1, 1);
MODULE_DEPEND(msm, qcom_scm, 1, 1, 1);
MODULE_DEPEND(msm, qcom_cmd_db, 1, 1, 1);
MODULE_DEPEND(msm, qcom_smmu, 1, 1, 1);
