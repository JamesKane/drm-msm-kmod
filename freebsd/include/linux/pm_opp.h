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


/* Operating performance points: a table per device (msm_freebsd_opp.c). */
#ifndef _MSM_FREEBSD_LINUX_PM_OPP_H_
#define	_MSM_FREEBSD_LINUX_PM_OPP_H_

#include <linux/types.h>
#include <linux/err.h>

struct device;
struct dev_pm_opp;

struct dev_pm_opp_config {
	const char * const	*clk_names;
	const unsigned int	*supported_hw;
	unsigned int		supported_hw_count;
};

int	devm_pm_opp_of_add_table(struct device *dev);
int	devm_pm_opp_set_clkname(struct device *dev, const char *name);
int	devm_pm_opp_set_supported_hw(struct device *dev,
	    const u32 *versions, unsigned int count);
int	dev_pm_opp_set_config(struct device *dev,
	    struct dev_pm_opp_config *config);
int	dev_pm_opp_add(struct device *dev, unsigned long freq,
	    unsigned long u_volt);
int	dev_pm_opp_get_opp_count(struct device *dev);
struct dev_pm_opp *dev_pm_opp_find_freq_exact(struct device *dev,
	    unsigned long freq, bool available);
struct dev_pm_opp *dev_pm_opp_find_freq_ceil(struct device *dev,
	    unsigned long *freq);
struct dev_pm_opp *dev_pm_opp_find_freq_floor(struct device *dev,
	    unsigned long *freq);
unsigned long dev_pm_opp_get_freq(struct dev_pm_opp *opp);
unsigned int dev_pm_opp_get_level(struct dev_pm_opp *opp);
void	dev_pm_opp_put(struct dev_pm_opp *opp);
int	dev_pm_opp_set_opp(struct device *dev, struct dev_pm_opp *opp);
int	dev_pm_opp_set_rate(struct device *dev, unsigned long freq);
int	dev_pm_opp_of_find_icc_paths(struct device *dev, void *table);

#endif
