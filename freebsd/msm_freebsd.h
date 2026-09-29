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

/* Internal interfaces of the FreeBSD glue for msm. */
#ifndef _MSM_FREEBSD_H_
#define	_MSM_FREEBSD_H_

#include <linux/of.h>
#include <linux/platform_device.h>

#define	MSM_FBSD_MAX_PDEVS	4

/* A register window or interrupt of a platform device the glue creates. */
struct msm_fbsd_res {
	const char	*name;
	u64		start;		/* physical address, or GSIV */
	u64		size;		/* 0 for an interrupt */
	int		acpi_rid;	/* for interrupts: ACPI rid, or -1 */
};

struct msm_fbsd_opp {
	unsigned long	hz;
	unsigned int	level;		/* RPMh level */
	unsigned int	peak_kbps;
};

struct msm_fbsd_pdev_desc {
	const char			*name;
	const char			*node;	/* its device tree node */
	const struct msm_fbsd_res	*res;
	const struct msm_fbsd_opp	*opps;
	/* SMMU stream IDs and masks, as Linux's devicetree has them. */
	u16				sid[2];
	u16				sid_mask[2];
	int				nsids;
};

struct msm_fbsd_soc {
	const char			*pep_hid;	/* ACPI \_SB.PEP0 */
	struct device_node		*machine;
	struct device_node		*nodes;		/* NULL name terminated */
	const struct msm_fbsd_pdev_desc	*pdevs;		/* NULL name terminated */
};

extern const struct msm_fbsd_soc *msm_fbsd_soc;
extern const struct msm_fbsd_soc msm_fbsd_sc8280xp;

int	msm_fbsd_pdev_add(struct platform_device *pdev);
void	msm_fbsd_pdev_del(struct platform_device *pdev);
const struct msm_fbsd_pdev_desc *msm_fbsd_pdev_desc(struct device *dev);
struct device;
device_t msm_fbsd_bsddev(void);

/* msm_freebsd_iommu.c */
struct qcom_smmu;
void	msm_fbsd_iommu_set_smmu(struct qcom_smmu *sc);

#endif
