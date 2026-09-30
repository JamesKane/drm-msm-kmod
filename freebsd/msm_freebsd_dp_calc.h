/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2012-2020, The Linux Foundation. All rights reserved.
 */

/*
 * The DisplayPort stream calculations of Linux's msm and Qualcomm clock
 * drivers, for msmfb's mode setting.
 */

#ifndef _MSM_FREEBSD_DP_CALC_H_
#define	_MSM_FREEBSD_DP_CALC_H_

#include <linux/types.h>

struct msm_dp_tu_calc_input {
	u64 lclk;        /* 162, 270, 540 and 810 */
	u64 pclk_khz;    /* in KHz */
	u64 hactive;     /* active h-width */
	u64 hporch;      /* bp + fp + pulse */
	int nlanes;      /* no.of.lanes */
	int bpp;         /* bits */
	int pixel_enc;   /* 444, 420, 422 */
	int dsc_en;     /* dsc on/off */
	int async_en;   /* async mode */
	int fec_en;     /* fec */
	int compress_ratio; /* 2:1 = 200, 3:1 = 300, 3.75:1 = 375 */
	int num_of_dsc_slices; /* number of slices per line */
};

struct msm_dp_vc_tu_mapping_table {
	u32 vic;
	u8 lanes;
	u8 lrate; /* DP_LINK_RATE -> 162(6), 270(10), 540(20), 810 (30) */
	u8 bpp;
	u8 valid_boundary_link;
	u16 delay_start_link;
	bool boundary_moderation_en;
	u8 valid_lower_boundary_link;
	u8 upper_boundary_count;
	u8 lower_boundary_count;
	u8 tu_size_minus1;
};


void	msm_fbsd_dp_calc_tu(struct msm_dp_tu_calc_input *in,
	    struct msm_dp_vc_tu_mapping_table *tu_table);
void	msm_fbsd_dp_calc_msa(u32 rate, u32 stream_rate_khz, u32 *mvid,
	    u32 *nvid);
void	msm_fbsd_dp_calc_pixel_mnd(unsigned long parent_rate,
	    unsigned long rate, u32 *m, u32 *n, u32 *d);

#endif
