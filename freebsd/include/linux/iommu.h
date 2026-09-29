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
 * The IOMMU interface msm uses, backed by qcom_smmu(4) in msm_freebsd_iommu.c.
 * Replaces LinuxKPI's stub.
 */
#ifndef _MSM_FREEBSD_LINUX_IOMMU_H_
#define	_MSM_FREEBSD_LINUX_IOMMU_H_

#include <linux/types.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/scatterlist.h>

#define	IOMMU_READ	(1 << 0)
#define	IOMMU_WRITE	(1 << 1)
#define	IOMMU_CACHE	(1 << 2)
#define	IOMMU_NOEXEC	(1 << 3)
#define	IOMMU_MMIO	(1 << 4)
#define	IOMMU_PRIV	(1 << 5)

#define	IOMMU_FAULT_READ	0x0
#define	IOMMU_FAULT_WRITE	0x1

struct iommu_domain;
typedef int (*iommu_fault_handler_t)(struct iommu_domain *, struct device *,
    unsigned long, int, void *);

struct iommu_domain_geometry {
	dma_addr_t	aperture_start;
	dma_addr_t	aperture_end;
	bool		force_aperture;
};

struct iommu_domain {
	unsigned long			pgsize_bitmap;
	struct iommu_domain_geometry	geometry;
	iommu_fault_handler_t		handler;
	void				*handler_token;
	unsigned long			quirks;
	void				*fbsd;		/* msm_freebsd_iommu.c */
};

struct iommu_iotlb_gather {
	unsigned long	start;
	unsigned long	end;
	size_t		pgsize;
};

struct iommu_flush_ops {
	void (*tlb_flush_all)(void *cookie);
	void (*tlb_flush_walk)(unsigned long iova, size_t size, size_t granule,
	    void *cookie);
	void (*tlb_add_page)(struct iommu_iotlb_gather *gather,
	    unsigned long iova, size_t granule, void *cookie);
};

bool	device_iommu_mapped(struct device *dev);
struct iommu_domain *iommu_paging_domain_alloc(struct device *dev);
void	iommu_domain_free(struct iommu_domain *domain);
int	iommu_attach_device(struct iommu_domain *domain, struct device *dev);
void	iommu_detach_device(struct iommu_domain *domain, struct device *dev);
int	iommu_map(struct iommu_domain *domain, unsigned long iova,
	    phys_addr_t paddr, size_t size, int prot, gfp_t gfp);
size_t	iommu_unmap(struct iommu_domain *domain, unsigned long iova,
	    size_t size);
ssize_t	iommu_map_sgtable(struct iommu_domain *domain, unsigned long iova,
	    struct sg_table *sgt, int prot);
void	iommu_flush_iotlb_all(struct iommu_domain *domain);
int	iommu_set_pgtable_quirks(struct iommu_domain *domain,
	    unsigned long quirks);
void	iommu_set_fault_handler(struct iommu_domain *domain,
	    iommu_fault_handler_t handler, void *token);

static inline struct iommu_domain *
iommu_get_domain_for_dev(struct device *dev __unused)
{
	return (NULL);
}

#endif
