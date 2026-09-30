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
 * devfreq: a device's frequency, scaled with its load by polling it under the
 * simple_ondemand policy; enough of Linux's for msm's GPU.
 */
#ifndef _MSM_FREEBSD_LINUX_DEVFREQ_H_
#define	_MSM_FREEBSD_LINUX_DEVFREQ_H_

#include <linux/types.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pm_opp.h>
#include <linux/workqueue.h>

struct device;

#define	DEVFREQ_GOV_SIMPLE_ONDEMAND	"simple_ondemand"
#define	DEVFREQ_FLAG_LEAST_UPPER_BOUND	0x1

struct devfreq_dev_status {
	unsigned long	total_time;
	unsigned long	busy_time;
	unsigned long	current_frequency;
	void		*private_data;
};

struct devfreq_dev_profile {
	unsigned long	initial_freq;
	unsigned int	polling_ms;
	int		timer;
	int		(*target)(struct device *, unsigned long *, u32);
	int		(*get_dev_status)(struct device *,
			    struct devfreq_dev_status *);
	int		(*get_cur_freq)(struct device *, unsigned long *);
	unsigned long	*freq_table;
	unsigned int	max_state;
	bool		is_cooling_device;
};

struct devfreq_simple_ondemand_data {
	unsigned int	upthreshold;
	unsigned int	downdifferential;
};

struct devfreq {
	struct devfreq_dev_profile *profile;
	struct device	*dev;
	struct mutex	lock;		/* the profile's callbacks; below */
	unsigned long	previous_freq;
	unsigned long	min_freq;	/* of the OPPs */
	unsigned long	max_freq;
	unsigned int	upthreshold;	/* busy %, to go to max_freq */
	unsigned int	downdifferential; /* % below, to go down */
	unsigned int	load;		/* busy % at the last poll */
	bool		suspended;
	struct delayed_work work;
	struct list_head link;
};

struct devfreq *devm_devfreq_add_device(struct device *dev,
	    struct devfreq_dev_profile *profile, const char *governor,
	    void *data);
struct dev_pm_opp *devfreq_recommended_opp(struct device *dev,
	    unsigned long *freq, u32 flags);
int	devfreq_suspend_device(struct devfreq *df);
int	devfreq_resume_device(struct devfreq *df);

#endif
