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
 * IOMMU domains for the glue's platform devices, on qcom_smmu(4): a domain
 * is a page table, and attaching a device gives it a context bank and
 * routes its streams (as the SoC description lists them) to it.
 *
 * Only one address space per domain (TTBR0): per-process page tables are
 * not provided yet, so msm uses one GPU address space for all processes.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/iommu.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/sizes.h>

#include <linux/adreno-smmu-priv.h>
#include <linux/io-pgtable.h>

#include <dev/qcom_smmu/qcom_smmu.h>

#include "msm_freebsd.h"

/*
 * As Linux's SMMU driver does for the Adreno GPU, the GPU's domain has
 * split page tables: its own, holding the kernel's mappings, translate the
 * top of the address space, and msm's per-process tables, made with
 * io-pgtable, the bottom.  The GPU switches between the per-process tables
 * itself, in context bank 0.
 */
struct msm_fbsd_domain {
	struct qcom_smmu_pt	*pt;
	struct qcom_smmu_cb	*cb;
	const struct msm_fbsd_pdev_desc *desc;
	struct adreno_smmu_priv	*adreno_smmu;	/* the GPU's, while attached */
	struct io_pgtable_cfg	ttbr1_cfg;
};

struct msm_fbsd_io_pgtable {
	struct io_pgtable_ops	ops;
	struct qcom_smmu_pt	*pt;
};

static u_int	msm_fbsd_map_flags(int prot);

static struct qcom_smmu *msm_fbsd_smmu;

void
msm_fbsd_iommu_set_smmu(struct qcom_smmu *sc)
{
	msm_fbsd_smmu = sc;
}

static struct msm_fbsd_domain *
to_fbsd(struct iommu_domain *domain)
{
	return (domain->fbsd);
}

bool
msm_fbsd_device_iommu_mapped(struct device *dev)
{
	const struct msm_fbsd_pdev_desc *desc = msm_fbsd_pdev_desc(dev);

	return (msm_fbsd_smmu != NULL && desc != NULL && desc->nsids > 0);
}

/* The SMMU's table walks are coherent (dma-coherent in the devicetree). */
bool
device_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	return (device_iommu_mapped(dev) && cap == IOMMU_CAP_CACHE_COHERENCY);
}

/* The GPU's adreno_smmu_priv, which msm uses for per-process page tables. */

static void
msm_fbsd_tlb_flush_all(void *cookie)
{
	struct msm_fbsd_domain *fd = __DECONST(struct msm_fbsd_domain *, cookie);

	if (fd->cb != NULL)
		(void)qcom_smmu_cb_tlb_inv(fd->cb);
}

static void
msm_fbsd_tlb_flush_walk(unsigned long iova __unused, size_t size __unused,
    size_t granule __unused, void *cookie)
{
	msm_fbsd_tlb_flush_all(cookie);
}

static void
msm_fbsd_tlb_add_page(struct iommu_iotlb_gather *gather __unused,
    unsigned long iova __unused, size_t granule __unused, void *cookie)
{
	msm_fbsd_tlb_flush_all(cookie);
}

static const struct iommu_flush_ops msm_fbsd_tlb_ops = {
	.tlb_flush_all = msm_fbsd_tlb_flush_all,
	.tlb_flush_walk = msm_fbsd_tlb_flush_walk,
	.tlb_add_page = msm_fbsd_tlb_add_page,
};

static const struct io_pgtable_cfg *
msm_fbsd_get_ttbr1_cfg(const void *cookie)
{
	const struct msm_fbsd_domain *fd = cookie;

	return (&fd->ttbr1_cfg);
}

/* Point TTBR0 at a per-process table, or with no table, turn it off. */
static int
msm_fbsd_set_ttbr0_cfg(const void *cookie, const struct io_pgtable_cfg *cfg)
{
	const struct msm_fbsd_domain *fd = cookie;

	return (-qcom_smmu_cb_set_ttbr0(fd->cb,
	    cfg != NULL ? cfg->arm_lpae_s1_cfg.ttbr : 0));
}

static void
msm_fbsd_adreno_smmu_init(struct msm_fbsd_domain *fd, struct device *dev)
{
	struct io_pgtable_cfg *cfg = &fd->ttbr1_cfg;

	memset(cfg, 0, sizeof(*cfg));
	cfg->quirks = IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg->pgsize_bitmap = SZ_4K;
	cfg->ias = 48;
	cfg->oas = 48;
	cfg->coherent_walk = true;
	cfg->tlb = &msm_fbsd_tlb_ops;
	cfg->iommu_dev = dev;
	cfg->arm_lpae_s1_cfg.ttbr = qcom_smmu_pt_root(fd->pt);

	/* msm_gpu_init() made the priv the device's driver data. */
	fd->adreno_smmu = dev_get_drvdata(dev);
	fd->adreno_smmu->cookie = fd;
	fd->adreno_smmu->get_ttbr1_cfg = msm_fbsd_get_ttbr1_cfg;
	fd->adreno_smmu->set_ttbr0_cfg = msm_fbsd_set_ttbr0_cfg;
}

/* io-pgtable, for msm's per-process page tables. */

static struct msm_fbsd_io_pgtable *
to_fbsd_pgtable(struct io_pgtable_ops *ops)
{
	return (container_of(ops, struct msm_fbsd_io_pgtable, ops));
}

static int
msm_fbsd_pgtable_map(struct io_pgtable_ops *ops, unsigned long iova,
    phys_addr_t paddr, size_t pgsize, size_t pgcount, int prot,
    gfp_t gfp __unused, size_t *mapped)
{
	int error;

	error = qcom_smmu_map(to_fbsd_pgtable(ops)->pt, iova, paddr,
	    pgsize * pgcount, msm_fbsd_map_flags(prot));
	if (error == 0)
		*mapped += pgsize * pgcount;
	return (-error);
}

static size_t
msm_fbsd_pgtable_unmap(struct io_pgtable_ops *ops, unsigned long iova,
    size_t pgsize, size_t pgcount, struct iommu_iotlb_gather *gather __unused)
{
	qcom_smmu_unmap(to_fbsd_pgtable(ops)->pt, iova, pgsize * pgcount);
	return (pgsize * pgcount);
}

struct io_pgtable_ops *
alloc_io_pgtable_ops(enum io_pgtable_fmt fmt, struct io_pgtable_cfg *cfg,
    void *cookie __unused)
{
	struct msm_fbsd_io_pgtable *pgt;

	if (fmt != ARM_64_LPAE_S1 ||
	    (cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1) != 0)
		return (NULL);
	pgt = kzalloc(sizeof(*pgt), GFP_KERNEL);
	if (pgt == NULL)
		return (NULL);
	pgt->pt = qcom_smmu_pt_create(0);
	pgt->ops.map_pages = msm_fbsd_pgtable_map;
	pgt->ops.unmap_pages = msm_fbsd_pgtable_unmap;
	cfg->pgsize_bitmap = SZ_4K;
	cfg->arm_lpae_s1_cfg.ttbr = qcom_smmu_pt_root(pgt->pt);
	return (&pgt->ops);
}

void
free_io_pgtable_ops(struct io_pgtable_ops *ops)
{
	struct msm_fbsd_io_pgtable *pgt;

	if (ops == NULL)
		return;
	pgt = to_fbsd_pgtable(ops);
	qcom_smmu_pt_destroy(pgt->pt);
	kfree(pgt);
}

struct iommu_domain *
iommu_paging_domain_alloc(struct device *dev)
{
	struct iommu_domain *domain;
	struct msm_fbsd_domain *fd;

	if (!device_iommu_mapped(dev))
		return (ERR_PTR(-ENODEV));
	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	fd = kzalloc(sizeof(*fd), GFP_KERNEL);
	domain->fbsd = fd;
	domain->pgsize_bitmap = SZ_4K;
	domain->geometry.force_aperture = true;
	if (msm_fbsd_pdev_desc(dev)->gpu) {
		fd->pt = qcom_smmu_pt_create(QCOM_SMMU_PT_UPPER);
		domain->geometry.aperture_start = ~0UL << 48;
		domain->geometry.aperture_end = ~0UL;
	} else {
		fd->pt = qcom_smmu_pt_create(0);
		domain->geometry.aperture_start = 0;
		domain->geometry.aperture_end = (1UL << 48) - 1;
	}
	return (domain);
}

void
iommu_domain_free(struct iommu_domain *domain)
{
	struct msm_fbsd_domain *fd = to_fbsd(domain);

	if (fd->cb != NULL)
		iommu_detach_device(domain, NULL);
	qcom_smmu_pt_destroy(fd->pt);
	kfree(fd);
	kfree(domain);
}

int
iommu_attach_device(struct iommu_domain *domain, struct device *dev)
{
	struct msm_fbsd_domain *fd = to_fbsd(domain);
	const struct msm_fbsd_pdev_desc *desc = msm_fbsd_pdev_desc(dev);
	int error, i;

	if (msm_fbsd_smmu == NULL || desc == NULL || fd->cb != NULL)
		return (-EINVAL);
	error = qcom_smmu_cb_alloc(msm_fbsd_smmu, fd->pt, &fd->cb);
	if (error != 0)
		return (-error);
	fd->desc = desc;
	if (desc->gpu) {
		/* The GPU switches page tables in bank 0 (adreno_hw_init()). */
		if (qcom_smmu_cb_index(fd->cb) != 0) {
			iommu_detach_device(domain, dev);
			return (-EBUSY);
		}
		msm_fbsd_adreno_smmu_init(fd, dev);
	}
	for (i = 0; i < desc->nsids; i++) {
		error = qcom_smmu_attach_stream(fd->cb, desc->sid[i],
		    desc->sid_mask[i]);
		if (error != 0) {
			iommu_detach_device(domain, dev);
			return (-error);
		}
	}
	return (0);
}

void
iommu_detach_device(struct iommu_domain *domain, struct device *dev __unused)
{
	struct msm_fbsd_domain *fd = to_fbsd(domain);
	int i;

	if (fd->cb == NULL)
		return;
	if (fd->adreno_smmu != NULL) {
		memset(fd->adreno_smmu, 0, sizeof(*fd->adreno_smmu));
		fd->adreno_smmu = NULL;
	}
	for (i = 0; i < fd->desc->nsids; i++)
		qcom_smmu_detach_stream(msm_fbsd_smmu, fd->desc->sid[i],
		    fd->desc->sid_mask[i]);
	qcom_smmu_cb_free(fd->cb);
	fd->cb = NULL;
}

static u_int
msm_fbsd_map_flags(int prot)
{
	u_int flags = 0;

	if ((prot & IOMMU_WRITE) == 0)
		flags |= QCOM_SMMU_READONLY;
	if ((prot & IOMMU_CACHE) == 0)
		flags |= QCOM_SMMU_UNCACHED;
	if ((prot & IOMMU_PRIV) != 0)
		flags |= QCOM_SMMU_PRIV;
	return (flags);
}

int
iommu_map(struct iommu_domain *domain, unsigned long iova, phys_addr_t paddr,
    size_t size, int prot, gfp_t gfp __unused)
{
	return (-qcom_smmu_map(to_fbsd(domain)->pt, iova, paddr, size,
	    msm_fbsd_map_flags(prot)));
}

size_t
iommu_unmap(struct iommu_domain *domain, unsigned long iova, size_t size)
{
	struct msm_fbsd_domain *fd = to_fbsd(domain);

	qcom_smmu_unmap(fd->pt, iova, size);
	if (fd->cb != NULL)
		(void)qcom_smmu_cb_tlb_inv(fd->cb);
	return (size);
}

ssize_t
iommu_map_sgtable(struct iommu_domain *domain, unsigned long iova,
    struct sg_table *sgt, int prot)
{
	struct msm_fbsd_domain *fd = to_fbsd(domain);
	struct scatterlist *sg;
	size_t mapped = 0;
	int error, i;

	for_each_sgtable_sg(sgt, sg, i) {
		error = qcom_smmu_map(fd->pt, iova + mapped, sg_phys(sg),
		    sg->length, msm_fbsd_map_flags(prot));
		if (error != 0) {
			if (mapped != 0)
				iommu_unmap(domain, iova, mapped);
			return (-error);
		}
		mapped += sg->length;
	}
	return (mapped);
}

void
iommu_flush_iotlb_all(struct iommu_domain *domain)
{
	struct msm_fbsd_domain *fd = to_fbsd(domain);

	if (fd->cb != NULL)
		(void)qcom_smmu_cb_tlb_inv(fd->cb);
}

/* TTBR1 (split address spaces) is not provided; everything is in TTBR0. */
int
iommu_set_pgtable_quirks(struct iommu_domain *domain, unsigned long quirks)
{
	domain->quirks = quirks;
	return (0);
}

/* Faults are not reported yet: context bank interrupts are not set up. */
void
iommu_set_fault_handler(struct iommu_domain *domain,
    iommu_fault_handler_t handler, void *token)
{
	domain->handler = handler;
	domain->handler_token = token;
}
