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
 * io-pgtable, as msm uses it for per-process GPU page tables: ARM_64_LPAE_S1
 * tables for TTBR0, on qcom_smmu(4) page tables (msm_freebsd_iommu.c).
 */
#ifndef _MSM_FREEBSD_LINUX_IO_PGTABLE_H_
#define	_MSM_FREEBSD_LINUX_IO_PGTABLE_H_

#include <linux/types.h>

struct iommu_flush_ops;
struct iommu_iotlb_gather;

enum io_pgtable_fmt {
	ARM_32_LPAE_S1,
	ARM_32_LPAE_S2,
	ARM_64_LPAE_S1,
	ARM_64_LPAE_S2,
};

#define	IO_PGTABLE_QUIRK_ARM_NS			(1UL << 0)
#define	IO_PGTABLE_QUIRK_NO_PERMS		(1UL << 1)
#define	IO_PGTABLE_QUIRK_ARM_MTK_EXT		(1UL << 3)
#define	IO_PGTABLE_QUIRK_ARM_TTBR1		(1UL << 5)
#define	IO_PGTABLE_QUIRK_ARM_OUTER_WBWA		(1UL << 6)

struct io_pgtable_cfg {
	unsigned long			quirks;
	unsigned long			pgsize_bitmap;
	unsigned int			ias;
	unsigned int			oas;
	bool				coherent_walk;
	const struct iommu_flush_ops	*tlb;
	struct device			*iommu_dev;
	union {
		struct {
			u64	ttbr;
			struct {
				u32 ips:3;
				u32 tg:2;
				u32 sh:2;
				u32 orgn:2;
				u32 irgn:2;
				u32 tsz:6;
			} tcr;
			u64	mair;
		} arm_lpae_s1_cfg;
	};
};

struct io_pgtable_ops {
	int (*map_pages)(struct io_pgtable_ops *ops, unsigned long iova,
	    phys_addr_t paddr, size_t pgsize, size_t pgcount, int prot,
	    gfp_t gfp, size_t *mapped);
	size_t (*unmap_pages)(struct io_pgtable_ops *ops, unsigned long iova,
	    size_t pgsize, size_t pgcount, struct iommu_iotlb_gather *gather);
	phys_addr_t (*iova_to_phys)(struct io_pgtable_ops *ops,
	    unsigned long iova);
};

struct io_pgtable_ops *alloc_io_pgtable_ops(enum io_pgtable_fmt fmt,
	    struct io_pgtable_cfg *cfg, void *cookie);
void	free_io_pgtable_ops(struct io_pgtable_ops *ops);

#endif
