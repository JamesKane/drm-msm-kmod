// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2020, The Linux Foundation. All rights reserved.
 * Copyright (C) 2009 emlix GmbH, Oskar Schirmer <oskar@scara.com>
 * Copyright (C) 2019 Trent Piepho <tpiepho@gmail.com>
 * Copyright (c) 2013, 2018, The Linux Foundation. All rights reserved.
 */

/*
 * The DisplayPort stream calculations msmfb's mode setting needs, from Linux:
 * the transfer unit calculation of msm's drivers/gpu/drm/msm/dp/dp_ctrl.c,
 * unchanged but for its entry point, which needs no controller, and debug
 * messages that go to pr_debug() for want of one; the MSA's Mvid and Nvid of
 * msm's dp_catalog.c; and the pixel clock's M/N/D of
 * drivers/clk/qcom/clk-rcg2.c, with lib/math/rational.c.
 */

#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <drm/display/drm_dp.h>
#include <drm/drm_fixed.h>

#include "msm_freebsd_dp_calc.h"


/*
 * The structure and few functions present below are IP/Hardware
 * specific implementation. Most of the implementation will not
 * have coding comments
 */
struct tu_algo_data {
	s64 lclk_fp;
	s64 pclk_fp;
	s64 lwidth;
	s64 lwidth_fp;
	s64 hbp_relative_to_pclk;
	s64 hbp_relative_to_pclk_fp;
	int nlanes;
	int bpp;
	int pixelEnc;
	int dsc_en;
	int async_en;
	int bpc;

	uint delay_start_link_extra_pixclk;
	int extra_buffer_margin;
	s64 ratio_fp;
	s64 original_ratio_fp;

	s64 err_fp;
	s64 n_err_fp;
	s64 n_n_err_fp;
	int tu_size;
	int tu_size_desired;
	int tu_size_minus1;

	int valid_boundary_link;
	s64 resulting_valid_fp;
	s64 total_valid_fp;
	s64 effective_valid_fp;
	s64 effective_valid_recorded_fp;
	int n_tus;
	int n_tus_per_lane;
	int paired_tus;
	int remainder_tus;
	int remainder_tus_upper;
	int remainder_tus_lower;
	int extra_bytes;
	int filler_size;
	int delay_start_link;

	int extra_pclk_cycles;
	int extra_pclk_cycles_in_link_clk;
	s64 ratio_by_tu_fp;
	s64 average_valid2_fp;
	int new_valid_boundary_link;
	int remainder_symbols_exist;
	int n_symbols;
	s64 n_remainder_symbols_per_lane_fp;
	s64 last_partial_tu_fp;
	s64 TU_ratio_err_fp;

	int n_tus_incl_last_incomplete_tu;
	int extra_pclk_cycles_tmp;
	int extra_pclk_cycles_in_link_clk_tmp;
	int extra_required_bytes_new_tmp;
	int filler_size_tmp;
	int lower_filler_size_tmp;
	int delay_start_link_tmp;

	bool boundary_moderation_en;
	int boundary_mod_lower_err;
	int upper_boundary_count;
	int lower_boundary_count;
	int i_upper_boundary_count;
	int i_lower_boundary_count;
	int valid_lower_boundary_link;
	int even_distribution_BF;
	int even_distribution_legacy;
	int even_distribution;
	int min_hblank_violated;
	s64 delay_start_time_fp;
	s64 hbp_time_fp;
	s64 hactive_time_fp;
	s64 diff_abs_fp;

	s64 ratio;
};

static int _tu_param_compare(s64 a, s64 b)
{
	u32 a_sign;
	u32 b_sign;
	s64 a_temp, b_temp, minus_1;

	if (a == b)
		return 0;

	minus_1 = drm_fixp_from_fraction(-1, 1);

	a_sign = (a >> 32) & 0x80000000 ? 1 : 0;

	b_sign = (b >> 32) & 0x80000000 ? 1 : 0;

	if (a_sign > b_sign)
		return 2;
	else if (b_sign > a_sign)
		return 1;

	if (!a_sign && !b_sign) { /* positive */
		if (a > b)
			return 1;
		else
			return 2;
	} else { /* negative */
		a_temp = drm_fixp_mul(a, minus_1);
		b_temp = drm_fixp_mul(b, minus_1);

		if (a_temp > b_temp)
			return 2;
		else
			return 1;
	}
}

static void msm_dp_panel_update_tu_timings(struct msm_dp_tu_calc_input *in,
					struct tu_algo_data *tu)
{
	int nlanes = in->nlanes;
	int dsc_num_slices = in->num_of_dsc_slices;
	int dsc_num_bytes  = 0;
	int numerator;
	s64 pclk_dsc_fp;
	s64 dwidth_dsc_fp;
	s64 hbp_dsc_fp;

	int tot_num_eoc_symbols = 0;
	int tot_num_hor_bytes   = 0;
	int tot_num_dummy_bytes = 0;
	int dwidth_dsc_bytes    = 0;
	int  eoc_bytes           = 0;

	s64 temp1_fp, temp2_fp, temp3_fp;

	tu->lclk_fp              = drm_fixp_from_fraction(in->lclk, 1);
	tu->pclk_fp              = drm_fixp_from_fraction(in->pclk_khz, 1000);
	tu->lwidth               = in->hactive;
	tu->hbp_relative_to_pclk = in->hporch;
	tu->nlanes               = in->nlanes;
	tu->bpp                  = in->bpp;
	tu->pixelEnc             = in->pixel_enc;
	tu->dsc_en               = in->dsc_en;
	tu->async_en             = in->async_en;
	tu->lwidth_fp            = drm_fixp_from_fraction(in->hactive, 1);
	tu->hbp_relative_to_pclk_fp = drm_fixp_from_fraction(in->hporch, 1);

	if (tu->pixelEnc == 420) {
		temp1_fp = drm_fixp_from_fraction(2, 1);
		tu->pclk_fp = drm_fixp_div(tu->pclk_fp, temp1_fp);
		tu->lwidth_fp = drm_fixp_div(tu->lwidth_fp, temp1_fp);
		tu->hbp_relative_to_pclk_fp =
				drm_fixp_div(tu->hbp_relative_to_pclk_fp, 2);
	}

	if (tu->pixelEnc == 422) {
		switch (tu->bpp) {
		case 24:
			tu->bpp = 16;
			tu->bpc = 8;
			break;
		case 30:
			tu->bpp = 20;
			tu->bpc = 10;
			break;
		default:
			tu->bpp = 16;
			tu->bpc = 8;
			break;
		}
	} else {
		tu->bpc = tu->bpp/3;
	}

	if (!in->dsc_en)
		goto fec_check;

	temp1_fp = drm_fixp_from_fraction(in->compress_ratio, 100);
	temp2_fp = drm_fixp_from_fraction(in->bpp, 1);
	temp3_fp = drm_fixp_div(temp2_fp, temp1_fp);
	temp2_fp = drm_fixp_mul(tu->lwidth_fp, temp3_fp);

	temp1_fp = drm_fixp_from_fraction(8, 1);
	temp3_fp = drm_fixp_div(temp2_fp, temp1_fp);

	numerator = drm_fixp2int(temp3_fp);

	dsc_num_bytes  = numerator / dsc_num_slices;
	eoc_bytes           = dsc_num_bytes % nlanes;
	tot_num_eoc_symbols = nlanes * dsc_num_slices;
	tot_num_hor_bytes   = dsc_num_bytes * dsc_num_slices;
	tot_num_dummy_bytes = (nlanes - eoc_bytes) * dsc_num_slices;

	if (dsc_num_bytes == 0)
		pr_info("incorrect no of bytes per slice=%d\n", dsc_num_bytes);

	dwidth_dsc_bytes = (tot_num_hor_bytes +
				tot_num_eoc_symbols +
				(eoc_bytes == 0 ? 0 : tot_num_dummy_bytes));

	dwidth_dsc_fp = drm_fixp_from_fraction(dwidth_dsc_bytes, 3);

	temp2_fp = drm_fixp_mul(tu->pclk_fp, dwidth_dsc_fp);
	temp1_fp = drm_fixp_div(temp2_fp, tu->lwidth_fp);
	pclk_dsc_fp = temp1_fp;

	temp1_fp = drm_fixp_div(pclk_dsc_fp, tu->pclk_fp);
	temp2_fp = drm_fixp_mul(tu->hbp_relative_to_pclk_fp, temp1_fp);
	hbp_dsc_fp = temp2_fp;

	/* output */
	tu->pclk_fp = pclk_dsc_fp;
	tu->lwidth_fp = dwidth_dsc_fp;
	tu->hbp_relative_to_pclk_fp = hbp_dsc_fp;

fec_check:
	if (in->fec_en) {
		temp1_fp = drm_fixp_from_fraction(976, 1000); /* 0.976 */
		tu->lclk_fp = drm_fixp_mul(tu->lclk_fp, temp1_fp);
	}
}

static void _tu_valid_boundary_calc(struct tu_algo_data *tu)
{
	s64 temp1_fp, temp2_fp, temp, temp1, temp2;
	int compare_result_1, compare_result_2, compare_result_3;

	temp1_fp = drm_fixp_from_fraction(tu->tu_size, 1);
	temp2_fp = drm_fixp_mul(tu->ratio_fp, temp1_fp);

	tu->new_valid_boundary_link = drm_fixp2int_ceil(temp2_fp);

	temp = (tu->i_upper_boundary_count *
				tu->new_valid_boundary_link +
				tu->i_lower_boundary_count *
				(tu->new_valid_boundary_link-1));
	tu->average_valid2_fp = drm_fixp_from_fraction(temp,
					(tu->i_upper_boundary_count +
					tu->i_lower_boundary_count));

	temp1_fp = drm_fixp_from_fraction(tu->bpp, 8);
	temp2_fp = tu->lwidth_fp;
	temp1_fp = drm_fixp_mul(temp2_fp, temp1_fp);
	temp2_fp = drm_fixp_div(temp1_fp, tu->average_valid2_fp);
	tu->n_tus = drm_fixp2int(temp2_fp);
	if ((temp2_fp & 0xFFFFFFFF) > 0xFFFFF000)
		tu->n_tus += 1;

	temp1_fp = drm_fixp_from_fraction(tu->n_tus, 1);
	temp2_fp = drm_fixp_mul(temp1_fp, tu->average_valid2_fp);
	temp1_fp = drm_fixp_from_fraction(tu->n_symbols, 1);
	temp2_fp = temp1_fp - temp2_fp;
	temp1_fp = drm_fixp_from_fraction(tu->nlanes, 1);
	temp2_fp = drm_fixp_div(temp2_fp, temp1_fp);
	tu->n_remainder_symbols_per_lane_fp = temp2_fp;

	temp1_fp = drm_fixp_from_fraction(tu->tu_size, 1);
	tu->last_partial_tu_fp =
			drm_fixp_div(tu->n_remainder_symbols_per_lane_fp,
					temp1_fp);

	if (tu->n_remainder_symbols_per_lane_fp != 0)
		tu->remainder_symbols_exist = 1;
	else
		tu->remainder_symbols_exist = 0;

	temp1_fp = drm_fixp_from_fraction(tu->n_tus, tu->nlanes);
	tu->n_tus_per_lane = drm_fixp2int(temp1_fp);

	tu->paired_tus = (int)((tu->n_tus_per_lane) /
					(tu->i_upper_boundary_count +
					 tu->i_lower_boundary_count));

	tu->remainder_tus = tu->n_tus_per_lane - tu->paired_tus *
						(tu->i_upper_boundary_count +
						tu->i_lower_boundary_count);

	if ((tu->remainder_tus - tu->i_upper_boundary_count) > 0) {
		tu->remainder_tus_upper = tu->i_upper_boundary_count;
		tu->remainder_tus_lower = tu->remainder_tus -
						tu->i_upper_boundary_count;
	} else {
		tu->remainder_tus_upper = tu->remainder_tus;
		tu->remainder_tus_lower = 0;
	}

	temp = tu->paired_tus * (tu->i_upper_boundary_count *
				tu->new_valid_boundary_link +
				tu->i_lower_boundary_count *
				(tu->new_valid_boundary_link - 1)) +
				(tu->remainder_tus_upper *
				 tu->new_valid_boundary_link) +
				(tu->remainder_tus_lower *
				(tu->new_valid_boundary_link - 1));
	tu->total_valid_fp = drm_fixp_from_fraction(temp, 1);

	if (tu->remainder_symbols_exist) {
		temp1_fp = tu->total_valid_fp +
				tu->n_remainder_symbols_per_lane_fp;
		temp2_fp = drm_fixp_from_fraction(tu->n_tus_per_lane, 1);
		temp2_fp = temp2_fp + tu->last_partial_tu_fp;
		temp1_fp = drm_fixp_div(temp1_fp, temp2_fp);
	} else {
		temp2_fp = drm_fixp_from_fraction(tu->n_tus_per_lane, 1);
		temp1_fp = drm_fixp_div(tu->total_valid_fp, temp2_fp);
	}
	tu->effective_valid_fp = temp1_fp;

	temp1_fp = drm_fixp_from_fraction(tu->tu_size, 1);
	temp2_fp = drm_fixp_mul(tu->ratio_fp, temp1_fp);
	tu->n_n_err_fp = tu->effective_valid_fp - temp2_fp;

	temp1_fp = drm_fixp_from_fraction(tu->tu_size, 1);
	temp2_fp = drm_fixp_mul(tu->ratio_fp, temp1_fp);
	tu->n_err_fp = tu->average_valid2_fp - temp2_fp;

	tu->even_distribution = tu->n_tus % tu->nlanes == 0 ? 1 : 0;

	temp1_fp = drm_fixp_from_fraction(tu->bpp, 8);
	temp2_fp = tu->lwidth_fp;
	temp1_fp = drm_fixp_mul(temp2_fp, temp1_fp);
	temp2_fp = drm_fixp_div(temp1_fp, tu->average_valid2_fp);

	if (temp2_fp)
		tu->n_tus_incl_last_incomplete_tu = drm_fixp2int_ceil(temp2_fp);
	else
		tu->n_tus_incl_last_incomplete_tu = 0;

	temp1 = 0;
	temp1_fp = drm_fixp_from_fraction(tu->tu_size, 1);
	temp2_fp = drm_fixp_mul(tu->original_ratio_fp, temp1_fp);
	temp1_fp = tu->average_valid2_fp - temp2_fp;
	temp2_fp = drm_fixp_from_fraction(tu->n_tus_incl_last_incomplete_tu, 1);
	temp1_fp = drm_fixp_mul(temp2_fp, temp1_fp);

	if (temp1_fp)
		temp1 = drm_fixp2int_ceil(temp1_fp);

	temp = tu->i_upper_boundary_count * tu->nlanes;
	temp1_fp = drm_fixp_from_fraction(tu->tu_size, 1);
	temp2_fp = drm_fixp_mul(tu->original_ratio_fp, temp1_fp);
	temp1_fp = drm_fixp_from_fraction(tu->new_valid_boundary_link, 1);
	temp2_fp = temp1_fp - temp2_fp;
	temp1_fp = drm_fixp_from_fraction(temp, 1);
	temp2_fp = drm_fixp_mul(temp1_fp, temp2_fp);

	if (temp2_fp)
		temp2 = drm_fixp2int_ceil(temp2_fp);
	else
		temp2 = 0;
	tu->extra_required_bytes_new_tmp = (int)(temp1 + temp2);

	temp1_fp = drm_fixp_from_fraction(8, tu->bpp);
	temp2_fp = drm_fixp_from_fraction(
	tu->extra_required_bytes_new_tmp, 1);
	temp1_fp = drm_fixp_mul(temp2_fp, temp1_fp);

	if (temp1_fp)
		tu->extra_pclk_cycles_tmp = drm_fixp2int_ceil(temp1_fp);
	else
		tu->extra_pclk_cycles_tmp = 0;

	temp1_fp = drm_fixp_from_fraction(tu->extra_pclk_cycles_tmp, 1);
	temp2_fp = drm_fixp_div(tu->lclk_fp, tu->pclk_fp);
	temp1_fp = drm_fixp_mul(temp1_fp, temp2_fp);

	if (temp1_fp)
		tu->extra_pclk_cycles_in_link_clk_tmp =
						drm_fixp2int_ceil(temp1_fp);
	else
		tu->extra_pclk_cycles_in_link_clk_tmp = 0;

	tu->filler_size_tmp = tu->tu_size - tu->new_valid_boundary_link;

	tu->lower_filler_size_tmp = tu->filler_size_tmp + 1;

	tu->delay_start_link_tmp = tu->extra_pclk_cycles_in_link_clk_tmp +
					tu->lower_filler_size_tmp +
					tu->extra_buffer_margin;

	temp1_fp = drm_fixp_from_fraction(tu->delay_start_link_tmp, 1);
	tu->delay_start_time_fp = drm_fixp_div(temp1_fp, tu->lclk_fp);

	compare_result_1 = _tu_param_compare(tu->n_n_err_fp, tu->diff_abs_fp);
	if (compare_result_1 == 2)
		compare_result_1 = 1;
	else
		compare_result_1 = 0;

	compare_result_2 = _tu_param_compare(tu->n_n_err_fp, tu->err_fp);
	if (compare_result_2 == 2)
		compare_result_2 = 1;
	else
		compare_result_2 = 0;

	compare_result_3 = _tu_param_compare(tu->hbp_time_fp,
					tu->delay_start_time_fp);
	if (compare_result_3 == 2)
		compare_result_3 = 0;
	else
		compare_result_3 = 1;

	if (((tu->even_distribution == 1) ||
			((tu->even_distribution_BF == 0) &&
			(tu->even_distribution_legacy == 0))) &&
			tu->n_err_fp >= 0 && tu->n_n_err_fp >= 0 &&
			compare_result_2 &&
			(compare_result_1 || (tu->min_hblank_violated == 1)) &&
			(tu->new_valid_boundary_link - 1) > 0 &&
			compare_result_3 &&
			(tu->delay_start_link_tmp <= 1023)) {
		tu->upper_boundary_count = tu->i_upper_boundary_count;
		tu->lower_boundary_count = tu->i_lower_boundary_count;
		tu->err_fp = tu->n_n_err_fp;
		tu->boundary_moderation_en = true;
		tu->tu_size_desired = tu->tu_size;
		tu->valid_boundary_link = tu->new_valid_boundary_link;
		tu->effective_valid_recorded_fp = tu->effective_valid_fp;
		tu->even_distribution_BF = 1;
		tu->delay_start_link = tu->delay_start_link_tmp;
	} else if (tu->boundary_mod_lower_err == 0) {
		compare_result_1 = _tu_param_compare(tu->n_n_err_fp,
							tu->diff_abs_fp);
		if (compare_result_1 == 2)
			tu->boundary_mod_lower_err = 1;
	}
}

void msm_fbsd_dp_calc_tu(struct msm_dp_tu_calc_input *in,
			 struct msm_dp_vc_tu_mapping_table *tu_table)
{
	struct tu_algo_data *tu;
	int compare_result_1, compare_result_2;
	u64 temp = 0;
	s64 temp_fp = 0, temp1_fp = 0, temp2_fp = 0;

	s64 LCLK_FAST_SKEW_fp = drm_fixp_from_fraction(6, 10000); /* 0.0006 */
	s64 const_p49_fp = drm_fixp_from_fraction(49, 100); /* 0.49 */
	s64 const_p56_fp = drm_fixp_from_fraction(56, 100); /* 0.56 */
	s64 RATIO_SCALE_fp = drm_fixp_from_fraction(1001, 1000);

	u8 DP_BRUTE_FORCE = 1;
	s64 BRUTE_FORCE_THRESHOLD_fp = drm_fixp_from_fraction(1, 10); /* 0.1 */
	uint EXTRA_PIXCLK_CYCLE_DELAY = 4;
	uint HBLANK_MARGIN = 4;

	tu = kzalloc(sizeof(*tu), GFP_KERNEL);
	if (!tu)
		return;

	msm_dp_panel_update_tu_timings(in, tu);

	tu->err_fp = drm_fixp_from_fraction(1000, 1); /* 1000 */

	temp1_fp = drm_fixp_from_fraction(4, 1);
	temp2_fp = drm_fixp_mul(temp1_fp, tu->lclk_fp);
	temp_fp = drm_fixp_div(temp2_fp, tu->pclk_fp);
	tu->extra_buffer_margin = drm_fixp2int_ceil(temp_fp);

	temp1_fp = drm_fixp_from_fraction(tu->bpp, 8);
	temp2_fp = drm_fixp_mul(tu->pclk_fp, temp1_fp);
	temp1_fp = drm_fixp_from_fraction(tu->nlanes, 1);
	temp2_fp = drm_fixp_div(temp2_fp, temp1_fp);
	tu->ratio_fp = drm_fixp_div(temp2_fp, tu->lclk_fp);

	tu->original_ratio_fp = tu->ratio_fp;
	tu->boundary_moderation_en = false;
	tu->upper_boundary_count = 0;
	tu->lower_boundary_count = 0;
	tu->i_upper_boundary_count = 0;
	tu->i_lower_boundary_count = 0;
	tu->valid_lower_boundary_link = 0;
	tu->even_distribution_BF = 0;
	tu->even_distribution_legacy = 0;
	tu->even_distribution = 0;
	tu->delay_start_time_fp = 0;

	tu->err_fp = drm_fixp_from_fraction(1000, 1);
	tu->n_err_fp = 0;
	tu->n_n_err_fp = 0;

	tu->ratio = drm_fixp2int(tu->ratio_fp);
	temp1_fp = drm_fixp_from_fraction(tu->nlanes, 1);
	div64_u64_rem(tu->lwidth_fp, temp1_fp, &temp2_fp);
	if (temp2_fp != 0 &&
			!tu->ratio && tu->dsc_en == 0) {
		tu->ratio_fp = drm_fixp_mul(tu->ratio_fp, RATIO_SCALE_fp);
		tu->ratio = drm_fixp2int(tu->ratio_fp);
		if (tu->ratio)
			tu->ratio_fp = drm_fixp_from_fraction(1, 1);
	}

	if (tu->ratio > 1)
		tu->ratio = 1;

	if (tu->ratio == 1)
		goto tu_size_calc;

	compare_result_1 = _tu_param_compare(tu->ratio_fp, const_p49_fp);
	if (!compare_result_1 || compare_result_1 == 1)
		compare_result_1 = 1;
	else
		compare_result_1 = 0;

	compare_result_2 = _tu_param_compare(tu->ratio_fp, const_p56_fp);
	if (!compare_result_2 || compare_result_2 == 2)
		compare_result_2 = 1;
	else
		compare_result_2 = 0;

	if (tu->dsc_en && compare_result_1 && compare_result_2) {
		HBLANK_MARGIN += 4;
		pr_debug("increase HBLANK_MARGIN to %d\n", HBLANK_MARGIN);
	}

tu_size_calc:
	for (tu->tu_size = 32; tu->tu_size <= 64; tu->tu_size++) {
		temp1_fp = drm_fixp_from_fraction(tu->tu_size, 1);
		temp2_fp = drm_fixp_mul(tu->ratio_fp, temp1_fp);
		temp = drm_fixp2int_ceil(temp2_fp);
		temp1_fp = drm_fixp_from_fraction(temp, 1);
		tu->n_err_fp = temp1_fp - temp2_fp;

		if (tu->n_err_fp < tu->err_fp) {
			tu->err_fp = tu->n_err_fp;
			tu->tu_size_desired = tu->tu_size;
		}
	}

	tu->tu_size_minus1 = tu->tu_size_desired - 1;

	temp1_fp = drm_fixp_from_fraction(tu->tu_size_desired, 1);
	temp2_fp = drm_fixp_mul(tu->ratio_fp, temp1_fp);
	tu->valid_boundary_link = drm_fixp2int_ceil(temp2_fp);

	temp1_fp = drm_fixp_from_fraction(tu->bpp, 8);
	temp2_fp = tu->lwidth_fp;
	temp2_fp = drm_fixp_mul(temp2_fp, temp1_fp);

	temp1_fp = drm_fixp_from_fraction(tu->valid_boundary_link, 1);
	temp2_fp = drm_fixp_div(temp2_fp, temp1_fp);
	tu->n_tus = drm_fixp2int(temp2_fp);
	if ((temp2_fp & 0xFFFFFFFF) > 0xFFFFF000)
		tu->n_tus += 1;

	tu->even_distribution_legacy = tu->n_tus % tu->nlanes == 0 ? 1 : 0;

	pr_debug("n_sym = %d, num_of_tus = %d\n",
			tu->valid_boundary_link, tu->n_tus);

	temp1_fp = drm_fixp_from_fraction(tu->tu_size_desired, 1);
	temp2_fp = drm_fixp_mul(tu->original_ratio_fp, temp1_fp);
	temp1_fp = drm_fixp_from_fraction(tu->valid_boundary_link, 1);
	temp2_fp = temp1_fp - temp2_fp;
	temp1_fp = drm_fixp_from_fraction(tu->n_tus + 1, 1);
	temp2_fp = drm_fixp_mul(temp1_fp, temp2_fp);

	temp = drm_fixp2int(temp2_fp);
	if (temp && temp2_fp)
		tu->extra_bytes = drm_fixp2int_ceil(temp2_fp);
	else
		tu->extra_bytes = 0;

	temp1_fp = drm_fixp_from_fraction(tu->extra_bytes, 1);
	temp2_fp = drm_fixp_from_fraction(8, tu->bpp);
	temp1_fp = drm_fixp_mul(temp1_fp, temp2_fp);

	if (temp && temp1_fp)
		tu->extra_pclk_cycles = drm_fixp2int_ceil(temp1_fp);
	else
		tu->extra_pclk_cycles = drm_fixp2int(temp1_fp);

	temp1_fp = drm_fixp_div(tu->lclk_fp, tu->pclk_fp);
	temp2_fp = drm_fixp_from_fraction(tu->extra_pclk_cycles, 1);
	temp1_fp = drm_fixp_mul(temp2_fp, temp1_fp);

	if (temp1_fp)
		tu->extra_pclk_cycles_in_link_clk = drm_fixp2int_ceil(temp1_fp);
	else
		tu->extra_pclk_cycles_in_link_clk = drm_fixp2int(temp1_fp);

	tu->filler_size = tu->tu_size_desired - tu->valid_boundary_link;

	temp1_fp = drm_fixp_from_fraction(tu->tu_size_desired, 1);
	tu->ratio_by_tu_fp = drm_fixp_mul(tu->ratio_fp, temp1_fp);

	tu->delay_start_link = tu->extra_pclk_cycles_in_link_clk +
				tu->filler_size + tu->extra_buffer_margin;

	tu->resulting_valid_fp =
			drm_fixp_from_fraction(tu->valid_boundary_link, 1);

	temp1_fp = drm_fixp_from_fraction(tu->tu_size_desired, 1);
	temp2_fp = drm_fixp_div(tu->resulting_valid_fp, temp1_fp);
	tu->TU_ratio_err_fp = temp2_fp - tu->original_ratio_fp;

	temp1_fp = drm_fixp_from_fraction(HBLANK_MARGIN, 1);
	temp1_fp = tu->hbp_relative_to_pclk_fp - temp1_fp;
	tu->hbp_time_fp = drm_fixp_div(temp1_fp, tu->pclk_fp);

	temp1_fp = drm_fixp_from_fraction(tu->delay_start_link, 1);
	tu->delay_start_time_fp = drm_fixp_div(temp1_fp, tu->lclk_fp);

	compare_result_1 = _tu_param_compare(tu->hbp_time_fp,
					tu->delay_start_time_fp);
	if (compare_result_1 == 2) /* if (hbp_time_fp < delay_start_time_fp) */
		tu->min_hblank_violated = 1;

	tu->hactive_time_fp = drm_fixp_div(tu->lwidth_fp, tu->pclk_fp);

	compare_result_2 = _tu_param_compare(tu->hactive_time_fp,
						tu->delay_start_time_fp);
	if (compare_result_2 == 2)
		tu->min_hblank_violated = 1;

	tu->delay_start_time_fp = 0;

	/* brute force */

	tu->delay_start_link_extra_pixclk = EXTRA_PIXCLK_CYCLE_DELAY;
	tu->diff_abs_fp = tu->resulting_valid_fp - tu->ratio_by_tu_fp;

	temp = drm_fixp2int(tu->diff_abs_fp);
	if (!temp && tu->diff_abs_fp <= 0xffff)
		tu->diff_abs_fp = 0;

	/* if(diff_abs < 0) diff_abs *= -1 */
	if (tu->diff_abs_fp < 0)
		tu->diff_abs_fp = drm_fixp_mul(tu->diff_abs_fp, -1);

	tu->boundary_mod_lower_err = 0;
	if ((tu->diff_abs_fp != 0 &&
			((tu->diff_abs_fp > BRUTE_FORCE_THRESHOLD_fp) ||
			 (tu->even_distribution_legacy == 0) ||
			 (DP_BRUTE_FORCE == 1))) ||
			(tu->min_hblank_violated == 1)) {
		do {
			tu->err_fp = drm_fixp_from_fraction(1000, 1);

			temp1_fp = drm_fixp_div(tu->lclk_fp, tu->pclk_fp);
			temp2_fp = drm_fixp_from_fraction(
					tu->delay_start_link_extra_pixclk, 1);
			temp1_fp = drm_fixp_mul(temp2_fp, temp1_fp);

			if (temp1_fp)
				tu->extra_buffer_margin =
					drm_fixp2int_ceil(temp1_fp);
			else
				tu->extra_buffer_margin = 0;

			temp1_fp = drm_fixp_from_fraction(tu->bpp, 8);
			temp1_fp = drm_fixp_mul(tu->lwidth_fp, temp1_fp);

			if (temp1_fp)
				tu->n_symbols = drm_fixp2int_ceil(temp1_fp);
			else
				tu->n_symbols = 0;

			for (tu->tu_size = 32; tu->tu_size <= 64; tu->tu_size++) {
				for (tu->i_upper_boundary_count = 1;
					tu->i_upper_boundary_count <= 15;
					tu->i_upper_boundary_count++) {
					for (tu->i_lower_boundary_count = 1;
						tu->i_lower_boundary_count <= 15;
						tu->i_lower_boundary_count++) {
						_tu_valid_boundary_calc(tu);
					}
				}
			}
			tu->delay_start_link_extra_pixclk--;
		} while (tu->boundary_moderation_en != true &&
			tu->boundary_mod_lower_err == 1 &&
			tu->delay_start_link_extra_pixclk != 0);

		if (tu->boundary_moderation_en == true) {
			temp1_fp = drm_fixp_from_fraction(
					(tu->upper_boundary_count *
					tu->valid_boundary_link +
					tu->lower_boundary_count *
					(tu->valid_boundary_link - 1)), 1);
			temp2_fp = drm_fixp_from_fraction(
					(tu->upper_boundary_count +
					tu->lower_boundary_count), 1);
			tu->resulting_valid_fp =
					drm_fixp_div(temp1_fp, temp2_fp);

			temp1_fp = drm_fixp_from_fraction(
					tu->tu_size_desired, 1);
			tu->ratio_by_tu_fp =
				drm_fixp_mul(tu->original_ratio_fp, temp1_fp);

			tu->valid_lower_boundary_link =
				tu->valid_boundary_link - 1;

			temp1_fp = drm_fixp_from_fraction(tu->bpp, 8);
			temp1_fp = drm_fixp_mul(tu->lwidth_fp, temp1_fp);
			temp2_fp = drm_fixp_div(temp1_fp,
						tu->resulting_valid_fp);
			tu->n_tus = drm_fixp2int(temp2_fp);

			tu->tu_size_minus1 = tu->tu_size_desired - 1;
			tu->even_distribution_BF = 1;

			temp1_fp =
				drm_fixp_from_fraction(tu->tu_size_desired, 1);
			temp2_fp =
				drm_fixp_div(tu->resulting_valid_fp, temp1_fp);
			tu->TU_ratio_err_fp = temp2_fp - tu->original_ratio_fp;
		}
	}

	temp2_fp = drm_fixp_mul(LCLK_FAST_SKEW_fp, tu->lwidth_fp);

	if (temp2_fp)
		temp = drm_fixp2int_ceil(temp2_fp);
	else
		temp = 0;

	temp1_fp = drm_fixp_from_fraction(tu->nlanes, 1);
	temp2_fp = drm_fixp_mul(tu->original_ratio_fp, temp1_fp);
	temp1_fp = drm_fixp_from_fraction(tu->bpp, 8);
	temp2_fp = drm_fixp_div(temp1_fp, temp2_fp);
	temp1_fp = drm_fixp_from_fraction(temp, 1);
	temp2_fp = drm_fixp_mul(temp1_fp, temp2_fp);
	temp = drm_fixp2int(temp2_fp);

	if (tu->async_en)
		tu->delay_start_link += (int)temp;

	temp1_fp = drm_fixp_from_fraction(tu->delay_start_link, 1);
	tu->delay_start_time_fp = drm_fixp_div(temp1_fp, tu->lclk_fp);

	/* OUTPUTS */
	tu_table->valid_boundary_link       = tu->valid_boundary_link;
	tu_table->delay_start_link          = tu->delay_start_link;
	tu_table->boundary_moderation_en    = tu->boundary_moderation_en;
	tu_table->valid_lower_boundary_link = tu->valid_lower_boundary_link;
	tu_table->upper_boundary_count      = tu->upper_boundary_count;
	tu_table->lower_boundary_count      = tu->lower_boundary_count;
	tu_table->tu_size_minus1            = tu->tu_size_minus1;

	pr_debug("TU: valid_boundary_link: %d\n",
				tu_table->valid_boundary_link);
	pr_debug("TU: delay_start_link: %d\n",
				tu_table->delay_start_link);
	pr_debug("TU: boundary_moderation_en: %d\n",
			tu_table->boundary_moderation_en);
	pr_debug("TU: valid_lower_boundary_link: %d\n",
			tu_table->valid_lower_boundary_link);
	pr_debug("TU: upper_boundary_count: %d\n",
			tu_table->upper_boundary_count);
	pr_debug("TU: lower_boundary_count: %d\n",
			tu_table->lower_boundary_count);
	pr_debug("TU: tu_size_minus1: %d\n",
			tu_table->tu_size_minus1);

	kfree(tu);
}

/* lib/math/rational.c */
static void rational_best_approximation(
	unsigned long given_numerator, unsigned long given_denominator,
	unsigned long max_numerator, unsigned long max_denominator,
	unsigned long *best_numerator, unsigned long *best_denominator)
{
	/* n/d is the starting rational, which is continually
	 * decreased each iteration using the Euclidean algorithm.
	 *
	 * dp is the value of d from the prior iteration.
	 *
	 * n2/d2, n1/d1, and n0/d0 are our successively more accurate
	 * approximations of the rational.  They are, respectively,
	 * the current, previous, and two prior iterations of it.
	 *
	 * a is current term of the continued fraction.
	 */
	unsigned long n, d, n0, d0, n1, d1, n2, d2;
	n = given_numerator;
	d = given_denominator;
	n0 = d1 = 0;
	n1 = d0 = 1;

	for (;;) {
		unsigned long dp, a;

		if (d == 0)
			break;
		/* Find next term in continued fraction, 'a', via
		 * Euclidean algorithm.
		 */
		dp = d;
		a = n / d;
		d = n % d;
		n = dp;

		/* Calculate the current rational approximation (aka
		 * convergent), n2/d2, using the term just found and
		 * the two prior approximations.
		 */
		n2 = n0 + a * n1;
		d2 = d0 + a * d1;

		/* If the current convergent exceeds the maxes, then
		 * return either the previous convergent or the
		 * largest semi-convergent, the final term of which is
		 * found below as 't'.
		 */
		if ((n2 > max_numerator) || (d2 > max_denominator)) {
			unsigned long t = ULONG_MAX;

			if (d1)
				t = (max_denominator - d0) / d1;
			if (n1)
				t = min(t, (max_numerator - n0) / n1);

			/* This tests if the semi-convergent is closer than the previous
			 * convergent.  If d1 is zero there is no previous convergent as this
			 * is the 1st iteration, so always choose the semi-convergent.
			 */
			if (!d1 || 2u * t > a || (2u * t == a && d0 * dp > d1 * d)) {
				n1 = n0 + t * n1;
				d1 = d0 + t * d1;
			}
			break;
		}
		n0 = n1;
		n1 = n2;
		d0 = d1;
		d1 = d2;
	}
	*best_numerator = n1;
	*best_denominator = d1;
}

/*
 * msm_dp_catalog_ctrl_config_msa() of dp_catalog.c, for RGB, returning what
 * it writes to REG_DP_SOFTWARE_MVID and REG_DP_SOFTWARE_NVID.
 */
void msm_fbsd_dp_calc_msa(u32 rate, u32 stream_rate_khz, u32 *mvidp,
			  u32 *nvidp)
{
	u32 pixel_m, pixel_n;
	u32 mvid, nvid, pixel_div = 0, dispcc_input_rate;
	u32 const nvid_fixed = DP_LINK_CONSTANT_N_VALUE;
	u32 const link_rate_hbr2 = 540000;
	u32 const link_rate_hbr3 = 810000;
	unsigned long den, num;

	if (rate == link_rate_hbr3)
		pixel_div = 6;
	else if (rate == 162000 || rate == 270000)
		pixel_div = 2;
	else if (rate == link_rate_hbr2)
		pixel_div = 4;
	else
		pr_err("Invalid pixel mux divider\n");

	dispcc_input_rate = (rate * 10) / pixel_div;

	rational_best_approximation(dispcc_input_rate, stream_rate_khz,
			(unsigned long)(1 << 16) - 1,
			(unsigned long)(1 << 16) - 1, &den, &num);

	den = ~(den - num);
	den = den & 0xFFFF;
	pixel_m = num;
	pixel_n = den;

	mvid = (pixel_m & 0xFFFF) * 5;
	nvid = (0xFFFF & (~pixel_n)) + (pixel_m & 0xFFFF);

	if (nvid < nvid_fixed) {
		u32 temp;

		temp = (nvid_fixed / nvid) * nvid;
		mvid = (nvid_fixed / nvid) * mvid;
		nvid = temp;
	}

	if (link_rate_hbr2 == rate)
		nvid *= 2;

	if (link_rate_hbr3 == rate)
		nvid *= 3;

	*mvidp = mvid;
	*nvidp = nvid;
}

/*
 * clk_rcg2_dp_set_rate() and __clk_rcg2_configure_mnd() of clk-rcg2.c, for
 * a 16-bit M/N/D counter: what goes into its M, N and D registers, or all
 * zero for the counter to be bypassed.
 */
void msm_fbsd_dp_calc_pixel_mnd(unsigned long parent_rate, unsigned long rate,
				u32 *m, u32 *n, u32 *d)
{
	u32 mask = GENMASK(15, 0);
	u32 d_val, n_minus_m;
	unsigned long num, den;

	rational_best_approximation(parent_rate, rate, mask, mask, &den, &num);

	if (num == den) {
		*m = *n = *d = 0;
		return;
	}

	*m = num & mask;
	*n = ~(den - num) & mask;

	/* Calculate 2d value */
	d_val = den;

	n_minus_m = den - num;
	n_minus_m *= 2;

	d_val = clamp_t(u32, d_val, num, n_minus_m);
	*d = ~d_val & mask;
}
