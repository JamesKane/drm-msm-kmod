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

#include <dev/qcom_smmu/qcom_smmu.h>

#include "msm_freebsd.h"

struct msm_fbsd_domain {
	struct qcom_smmu_pt	*pt;
	struct qcom_smmu_cb	*cb;
	const struct msm_fbsd_pdev_desc *desc;
};

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

struct iommu_domain *
iommu_paging_domain_alloc(struct device *dev)
{
	struct iommu_domain *domain;
	struct msm_fbsd_domain *fd;

	if (!device_iommu_mapped(dev))
		return (ERR_PTR(-ENODEV));
	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	fd = kzalloc(sizeof(*fd), GFP_KERNEL);
	fd->pt = qcom_smmu_pt_create();
	domain->fbsd = fd;
	domain->pgsize_bitmap = SZ_4K;
	domain->geometry.aperture_start = 0;
	domain->geometry.aperture_end = (1ULL << 48) - 1;
	domain->geometry.force_aperture = true;
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
