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
 * description, and provides Linux's request_irq() for them on top of
 * msm_freebsd_bus.c.
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

#define	MSM_FBSD_MAX_IRQS	4

struct msm_fbsd_dev {
	struct platform_device		pdev;
	const struct msm_fbsd_pdev_desc	*desc;
	struct resource			res[8];
};

static struct msm_fbsd_irq {
	unsigned int	irq;		/* the GSIV */
	irq_handler_t	handler;
	void		*arg;
	int		handle;		/* msm_freebsd_bus.c's */
} msm_fbsd_irqs[MSM_FBSD_MAX_IRQS];

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

bool
msm_fbsd_linux_soc_supported(const char *pep_hid)
{
	int i;

	for (i = 0; i < nitems(msm_fbsd_socs); i++)
		if (strcmp(msm_fbsd_socs[i]->pep_hid, pep_hid) == 0)
			return (true);
	return (false);
}

/* Interrupts */

static void
msm_fbsd_intr(void *arg)
{
	struct msm_fbsd_irq *irq = arg;

	linux_set_current(curthread);
	(void)irq->handler(irq->irq, irq->arg);
}

/* The interrupt's ACPI rid in the SoC description, or -1. */
static int
msm_fbsd_irq_rid(unsigned int irqno, bool *found)
{
	const struct msm_fbsd_res *r;
	int i;

	*found = false;
	for (i = 0; i < msm_fbsd_nfdevs; i++)
		for (r = msm_fbsd_fdevs[i]->desc->res; r != NULL &&
		    r->name != NULL; r++)
			if (r->size == 0 && r->start == irqno) {
				*found = true;
				return (r->acpi_rid);
			}
	return (-1);
}

int
msm_fbsd_request_irq(struct device *dev __unused, unsigned int irqno,
    irq_handler_t handler, unsigned long flags, const char *name __unused,
    void *arg)
{
	struct msm_fbsd_irq *irq;
	bool found;
	int i, rid, h;

	rid = msm_fbsd_irq_rid(irqno, &found);
	if (!found)
		return (-ENXIO);
	for (i = 0; i < MSM_FBSD_MAX_IRQS; i++)
		if (msm_fbsd_irqs[i].handler == NULL)
			break;
	if (i == MSM_FBSD_MAX_IRQS)
		return (-ENOSPC);
	irq = &msm_fbsd_irqs[i];
	irq->irq = irqno;
	irq->handler = handler;
	irq->arg = arg;
	h = msm_fbsd_bus_irq_alloc(irqno, rid, msm_fbsd_intr, irq,
	    (flags & IRQF_NO_AUTOEN) == 0);
	if (h < 0) {
		irq->handler = NULL;
		return (h);
	}
	irq->handle = h;
	return (0);
}

struct msm_fbsd_devm_irq {
	unsigned int	irq;
	void		*arg;
};

static void
msm_fbsd_devm_irq_release(struct device *dev __unused, void *res)
{
	struct msm_fbsd_devm_irq *dr = res;

	msm_fbsd_free_irq(dr->irq, dr->arg);
}

/* request_irq() whose interrupt is freed with the device's devres. */
int
msm_fbsd_devm_request_irq(struct device *dev, unsigned int irqno,
    irq_handler_t handler, unsigned long flags, const char *name, void *arg)
{
	struct msm_fbsd_devm_irq *dr;
	int error;

	dr = devres_alloc(msm_fbsd_devm_irq_release, sizeof(*dr), GFP_KERNEL);
	if (dr == NULL)
		return (-ENOMEM);
	error = msm_fbsd_request_irq(dev, irqno, handler, flags, name, arg);
	if (error != 0) {
		devres_free(dr);
		return (error);
	}
	dr->irq = irqno;
	dr->arg = arg;
	devres_add(dev, dr);
	return (0);
}

static struct msm_fbsd_irq *
msm_fbsd_irq_find(unsigned int irqno, void *arg)
{
	int i;

	for (i = 0; i < MSM_FBSD_MAX_IRQS; i++)
		if (msm_fbsd_irqs[i].handler != NULL &&
		    msm_fbsd_irqs[i].irq == irqno &&
		    (arg == NULL || msm_fbsd_irqs[i].arg == arg))
			return (&msm_fbsd_irqs[i]);
	return (NULL);
}

void
msm_fbsd_free_irq(unsigned int irqno, void *arg)
{
	struct msm_fbsd_irq *irq;

	if ((irq = msm_fbsd_irq_find(irqno, arg)) == NULL)
		return;
	msm_fbsd_bus_irq_free(irq->handle);
	irq->handler = NULL;
}

void
msm_fbsd_enable_irq(unsigned int irqno)
{
	struct msm_fbsd_irq *irq;

	if ((irq = msm_fbsd_irq_find(irqno, NULL)) != NULL)
		(void)msm_fbsd_bus_irq_enable(irq->handle);
}

void
msm_fbsd_disable_irq(unsigned int irqno)
{
	struct msm_fbsd_irq *irq;

	if ((irq = msm_fbsd_irq_find(irqno, NULL)) != NULL)
		msm_fbsd_bus_irq_disable(irq->handle);
}

/* Platform devices */

static struct msm_fbsd_dev *
msm_fbsd_dev_create(device_t dev, const struct msm_fbsd_pdev_desc *desc)
{
	struct msm_fbsd_dev *fdev;
	struct device_node *np;
	const struct msm_fbsd_res *r;
	struct device *ldev;
	int n;

	fdev = kzalloc(sizeof(*fdev), GFP_KERNEL);
	fdev->desc = desc;
	fdev->pdev.name = desc->name;
	fdev->pdev.id = -1;
	for (np = msm_fbsd_soc->nodes; desc->node != NULL && np->name != NULL;
	    np++)
		if (strcmp(np->name, desc->node) == 0)
			fdev->pdev.of_node = np;
	for (n = 0, r = desc->res; r != NULL && r->name != NULL &&
	    n < nitems(fdev->res); r++, n++) {
		fdev->res[n].name = r->name;
		fdev->res[n].start = r->start;
		if (r->size != 0) {
			fdev->res[n].end = r->start + r->size - 1;
			fdev->res[n].flags = IORESOURCE_MEM;
		} else {
			fdev->res[n].end = r->start;
			fdev->res[n].flags = IORESOURCE_IRQ;
		}
	}
	fdev->pdev.resource = fdev->res;
	fdev->pdev.num_resources = n;

	ldev = &fdev->pdev.dev;
	ldev->parent = &linux_root_device;
	for (n = 0; desc->parent != NULL && n < msm_fbsd_nfdevs; n++)
		if (strcmp(msm_fbsd_fdevs[n]->desc->name, desc->parent) == 0)
			ldev->parent = &msm_fbsd_fdevs[n]->pdev.dev;
	ldev->bsddev = dev;
	spin_lock_init(&ldev->devres_lock);
	INIT_LIST_HEAD(&ldev->devres_head);
	INIT_LIST_HEAD(&ldev->irqents);
	kobject_init(&ldev->kobj, &linux_dev_ktype);
	kobject_set_name(&ldev->kobj, "%s", desc->name);
	if (linux_dma_dev_init(ldev) != 0) {
		kfree(fdev);
		return (NULL);
	}
	return (fdev);
}

int
msm_fbsd_linux_attach(device_t dev, const char *pep_hid,
    struct qcom_smmu *smmu)
{
	const struct msm_fbsd_pdev_desc *desc;
	int error, i;

	linux_set_current(curthread);
	for (i = 0; i < nitems(msm_fbsd_socs); i++)
		if (strcmp(msm_fbsd_socs[i]->pep_hid, pep_hid) == 0)
			msm_fbsd_soc = msm_fbsd_socs[i];
	msm_fbsd_dev = dev;
	msm_fbsd_iommu_set_smmu(smmu);

	/* Suppliers first: the GMU, the GPU, then the DRM device. */
	for (desc = msm_fbsd_soc->pdevs; desc->name != NULL; desc++)
		;
	while (desc-- != msm_fbsd_soc->pdevs) {
		msm_fbsd_fdevs[msm_fbsd_nfdevs] = msm_fbsd_dev_create(dev, desc);
		if (msm_fbsd_fdevs[msm_fbsd_nfdevs] == NULL)
			return (-ENOMEM);
		msm_fbsd_nfdevs++;
		error = msm_fbsd_pdev_add(
		    &msm_fbsd_fdevs[msm_fbsd_nfdevs - 1]->pdev);
		if (error != 0)
			return (-error);
	}
	return (0);
}

void
msm_fbsd_linux_detach(void)
{
	int i;

	linux_set_current(curthread);
	while (msm_fbsd_nfdevs > 0) {
		msm_fbsd_nfdevs--;
		msm_fbsd_pdev_del(&msm_fbsd_fdevs[msm_fbsd_nfdevs]->pdev);
		linux_dma_dev_uninit(&msm_fbsd_fdevs[msm_fbsd_nfdevs]->pdev.dev);
		kfree(msm_fbsd_fdevs[msm_fbsd_nfdevs]);
		msm_fbsd_fdevs[msm_fbsd_nfdevs] = NULL;
	}
	/* Interrupts requested with request_irq() but never freed. */
	for (i = 0; i < MSM_FBSD_MAX_IRQS; i++)
		if (msm_fbsd_irqs[i].handler != NULL) {
			msm_fbsd_bus_irq_free(msm_fbsd_irqs[i].handle);
			msm_fbsd_irqs[i].handler = NULL;
		}
	msm_fbsd_rpm_fini();
	msm_fbsd_iommu_set_smmu(NULL);
	msm_fbsd_dev = NULL;
}
