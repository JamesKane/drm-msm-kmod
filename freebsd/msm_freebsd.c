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
 * The Linux side of the glue: creates the platform devices msm's drivers
 * attach to (the GMU, the GPU and a headless "msm" DRM device) from the SoC
 * description.  Their interrupts are the ACPI device's, which LinuxKPI's
 * request_irq() finds by number.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/kobject.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "msm_freebsd.h"
#include "msm_freebsd_bus.h"

struct msm_fbsd_dev {
	struct platform_device		pdev;
	const struct msm_fbsd_pdev_desc	*desc;
	struct resource			res[8];
};


static const struct msm_fbsd_soc *const msm_fbsd_socs[] = {
	&msm_fbsd_sc8280xp,
};

const struct msm_fbsd_soc *msm_fbsd_soc;
static device_t msm_fbsd_dev;
static struct msm_fbsd_dev *msm_fbsd_fdevs[MSM_FBSD_MAX_PDEVS];
static int msm_fbsd_nfdevs;

device_t
msm_fbsd_bsddev(void)
{
	return (msm_fbsd_dev);
}

const struct msm_fbsd_pdev_desc *
msm_fbsd_pdev_desc(struct device *dev)
{
	int i;

	for (i = 0; i < msm_fbsd_nfdevs; i++)
		if (&msm_fbsd_fdevs[i]->pdev.dev == dev)
			return (msm_fbsd_fdevs[i]->desc);
	return (NULL);
}

/* The i'th supported SoC's power controller HID and GPU CC address. */
bool
msm_fbsd_linux_soc(int i, const char **pep_hid, uint64_t *gpucc_pa)
{
	if (i < 0 || i >= nitems(msm_fbsd_socs))
		return (false);
	*pep_hid = msm_fbsd_socs[i]->pep_hid;
	*gpucc_pa = msm_fbsd_socs[i]->gpucc_pa;
	return (true);
}

/* Platform devices */

static void
msm_fbsd_dev_release(struct device *dev)
{
	kfree(container_of(to_platform_device(dev), struct msm_fbsd_dev,
	    pdev));
}

static struct msm_fbsd_dev *
msm_fbsd_dev_create(device_t dev, const struct msm_fbsd_pdev_desc *desc)
{
	struct msm_fbsd_dev *fdev;
	struct device_node *np;
	const struct msm_fbsd_res *r;
	struct device *ldev;
	int irq, n;

	fdev = kzalloc(sizeof(*fdev), GFP_KERNEL);
	fdev->desc = desc;
	fdev->pdev.name = desc->name;
	fdev->pdev.id = -1;
	for (np = msm_fbsd_soc->nodes; desc->node != NULL && np->name != NULL;
	    np++)
		if (strcmp(np->name, desc->node) == 0)
			fdev->pdev.dev.of_node = np;
	for (n = 0, r = desc->res; r != NULL && r->name != NULL &&
	    n < nitems(fdev->res); r++, n++) {
		fdev->res[n].name = r->name;
		if (r->size != 0) {
			fdev->res[n].start = r->start;
			fdev->res[n].end = r->start + r->size - 1;
			fdev->res[n].flags = IORESOURCE_MEM;
		} else {
			/* The FreeBSD interrupt number of the GSIV. */
			irq = msm_fbsd_bus_irq(r->start, r->acpi_rid);
			if (irq < 0) {
				kfree(fdev);
				return (NULL);
			}
			fdev->res[n].start = fdev->res[n].end = irq;
			fdev->res[n].flags = IORESOURCE_IRQ;
		}
	}
	fdev->pdev.resource = fdev->res;
	fdev->pdev.num_resources = n;

	/* LinuxKPI names it and sets up DMA through the ACPI device. */
	ldev = &fdev->pdev.dev;
	for (n = 0; desc->parent != NULL && n < msm_fbsd_nfdevs; n++)
		if (strcmp(msm_fbsd_fdevs[n]->desc->name, desc->parent) == 0)
			ldev->parent = &msm_fbsd_fdevs[n]->pdev.dev;
	ldev->bsddev = dev;
	ldev->release = msm_fbsd_dev_release;
	return (fdev);
}

int
msm_fbsd_linux_attach(device_t dev, int soc, struct qcom_smmu *smmu)
{
	const struct msm_fbsd_pdev_desc *desc;
	struct msm_fbsd_dev *fdev;
	int error;

	linux_set_current(curthread);
	msm_fbsd_soc = msm_fbsd_socs[soc];
	msm_fbsd_dev = dev;
	msm_fbsd_iommu_set_smmu(smmu);

	/* Suppliers first: the GMU, the GPU, then the DRM device. */
	for (desc = msm_fbsd_soc->pdevs; desc->name != NULL; desc++)
		;
	while (desc-- != msm_fbsd_soc->pdevs) {
		fdev = msm_fbsd_dev_create(dev, desc);
		if (fdev == NULL)
			return (-ENXIO);
		/* of_find_device_by_node() finds it, even from probe(). */
		if (fdev->pdev.dev.of_node != NULL)
			fdev->pdev.dev.of_node->pdev = &fdev->pdev;
		msm_fbsd_fdevs[msm_fbsd_nfdevs++] = fdev;
		error = platform_device_register(&fdev->pdev);
		if (error != 0) {
			msm_fbsd_nfdevs--;
			if (fdev->pdev.dev.of_node != NULL)
				fdev->pdev.dev.of_node->pdev = NULL;
			platform_device_put(&fdev->pdev);
			return (error);
		}
	}
	return (0);
}

void
msm_fbsd_linux_detach(void)
{
	struct msm_fbsd_dev *fdev;
	struct device_node *np;

	linux_set_current(curthread);
	while (msm_fbsd_nfdevs > 0) {
		fdev = msm_fbsd_fdevs[--msm_fbsd_nfdevs];
		msm_fbsd_fdevs[msm_fbsd_nfdevs] = NULL;
		np = fdev->pdev.dev.of_node;
		platform_device_unregister(&fdev->pdev);
		if (np != NULL)
			np->pdev = NULL;
	}
	msm_fbsd_iommu_set_smmu(NULL);
	msm_fbsd_dev = NULL;
}
