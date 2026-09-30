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
 * devfreq for msm's GPU: every polling interval, the device reports how busy
 * it was, and the simple_ondemand policy picks its next frequency.  Above
 * upthreshold % busy it goes to the top; within downdifferential % below
 * that it stays; below, it goes to the frequency at which the same work
 * would keep it (upthreshold - downdifferential / 2) % busy.  The result is
 * held to the OPPs and to the device's PM QoS minimum, which msm raises for
 * a moment when the GPU wakes, and the profile's target() sets it.  Polling
 * stops while the device is suspended.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

#include <linux/device.h>
#include <linux/devfreq.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

static bool msm_fbsd_devfreq_on = true;
SYSCTL_BOOL(_hw_msm, OID_AUTO, devfreq, CTLFLAG_RWTUN,
    &msm_fbsd_devfreq_on, 0,
    "Scale the GPU's frequency with its load (else the fastest)");

static DEFINE_MUTEX(msm_fbsd_devfreq_list_lock);	/* the lists */
static LIST_HEAD(msm_fbsd_devfreqs);
static struct dev_pm_qos_request *msm_fbsd_qos_reqs;

/* The highest minimum the device's requests ask for, in Hz. */
static unsigned long
msm_fbsd_qos_min(struct device *dev)
{
	struct dev_pm_qos_request *req;
	unsigned long min;

	min = 0;
	mutex_lock(&msm_fbsd_devfreq_list_lock);
	for (req = msm_fbsd_qos_reqs; req != NULL; req = req->next)
		if (req->dev == dev && req->value > 0)
			min = max(min, (unsigned long)req->value * 1000);
	mutex_unlock(&msm_fbsd_devfreq_list_lock);
	return (min);
}

/* The simple_ondemand policy. */
static unsigned long
msm_fbsd_devfreq_policy(struct devfreq *df, const struct devfreq_dev_status *st)
{
	uint64_t busy, total;
	unsigned long cur;

	busy = st->busy_time;
	total = st->total_time;
	cur = st->current_frequency != 0 ? st->current_frequency :
	    df->previous_freq;
	if (!READ_ONCE(msm_fbsd_devfreq_on))
		return (df->max_freq);
	if (total == 0 || cur == 0 || busy * 100 > total * df->upthreshold)
		return (df->max_freq);
	if (busy * 100 > total * (df->upthreshold - df->downdifferential))
		return (cur);
	return (cur * busy * 100 /
	    (total * (df->upthreshold - df->downdifferential / 2)));
}

/* Poll the device and set its frequency; with df->lock held. */
static void
msm_fbsd_devfreq_update(struct devfreq *df)
{
	struct devfreq_dev_status st;
	unsigned long freq;

	memset(&st, 0, sizeof(st));
	if (df->profile->get_dev_status(df->dev, &st) != 0)
		return;
	df->load = st.total_time != 0 ?
	    (uint64_t)st.busy_time * 100 / st.total_time : 0;
	freq = msm_fbsd_devfreq_policy(df, &st);
	freq = max(freq, max(df->min_freq, msm_fbsd_qos_min(df->dev)));
	freq = min(freq, df->max_freq);
	if (df->profile->target(df->dev, &freq, 0) == 0)
		df->previous_freq = freq;
}

static void
msm_fbsd_devfreq_work(struct work_struct *work)
{
	struct devfreq *df = container_of(work, struct devfreq, work.work);

	mutex_lock(&df->lock);
	if (!df->suspended) {
		msm_fbsd_devfreq_update(df);
		queue_delayed_work(system_wq, &df->work,
		    msecs_to_jiffies(df->profile->polling_ms));
	}
	mutex_unlock(&df->lock);
}

static void
msm_fbsd_devfreq_release(void *arg)
{
	struct devfreq *df = arg;

	mutex_lock(&msm_fbsd_devfreq_list_lock);
	list_del(&df->link);
	mutex_unlock(&msm_fbsd_devfreq_list_lock);
	mutex_lock(&df->lock);
	df->suspended = true;
	mutex_unlock(&df->lock);
	cancel_delayed_work_sync(&df->work);
	mutex_destroy(&df->lock);
	kfree(df);
}

struct devfreq *
devm_devfreq_add_device(struct device *dev, struct devfreq_dev_profile *profile,
    const char *governor, void *data)
{
	struct devfreq_simple_ondemand_data *od = data;
	struct devfreq *df;
	unsigned long freq;
	int error;

	if (strcmp(governor, DEVFREQ_GOV_SIMPLE_ONDEMAND) != 0 ||
	    profile->polling_ms == 0)
		return (ERR_PTR(-EINVAL));
	df = kzalloc(sizeof(*df), GFP_KERNEL);
	if (df == NULL)
		return (ERR_PTR(-ENOMEM));
	df->profile = profile;
	df->dev = dev;
	mutex_init(&df->lock);
	INIT_DELAYED_WORK(&df->work, msm_fbsd_devfreq_work);
	/* Linux's defaults, for data that leaves them out. */
	df->upthreshold = od != NULL && od->upthreshold != 0 ?
	    od->upthreshold : 90;
	df->downdifferential = od != NULL && od->downdifferential != 0 ?
	    od->downdifferential : 5;
	if (df->downdifferential >= df->upthreshold)
		df->downdifferential = df->upthreshold / 2;
	freq = 0;
	if (!IS_ERR(dev_pm_opp_find_freq_ceil(dev, &freq)))
		df->min_freq = freq;
	freq = ULONG_MAX;
	if (!IS_ERR(dev_pm_opp_find_freq_floor(dev, &freq)))
		df->max_freq = freq;
	if (df->min_freq == 0 || df->max_freq == 0) {
		mutex_destroy(&df->lock);
		kfree(df);
		return (ERR_PTR(-ENODEV));
	}
	df->previous_freq = profile->initial_freq != 0 ?
	    profile->initial_freq : df->max_freq;
	/* Polled once resumed, like the device. */
	df->suspended = true;
	mutex_lock(&msm_fbsd_devfreq_list_lock);
	list_add_tail(&df->link, &msm_fbsd_devfreqs);
	mutex_unlock(&msm_fbsd_devfreq_list_lock);
	error = devm_add_action_or_reset(dev, msm_fbsd_devfreq_release, df);
	if (error != 0)
		return (ERR_PTR(error));
	return (df);
}

/*
 * The OPP for a frequency, which it rounds to: the lowest at or above it,
 * or with DEVFREQ_FLAG_LEAST_UPPER_BOUND the highest at or below it; the
 * nearest if there is none such.
 */
struct dev_pm_opp *
devfreq_recommended_opp(struct device *dev, unsigned long *freq, u32 flags)
{
	struct dev_pm_opp *opp;
	bool floor;

	floor = (flags & DEVFREQ_FLAG_LEAST_UPPER_BOUND) != 0;
	opp = floor ? dev_pm_opp_find_freq_floor(dev, freq) :
	    dev_pm_opp_find_freq_ceil(dev, freq);
	if (opp == ERR_PTR(-ERANGE))
		opp = floor ? dev_pm_opp_find_freq_ceil(dev, freq) :
		    dev_pm_opp_find_freq_floor(dev, freq);
	return (opp);
}

int
devfreq_suspend_device(struct devfreq *df)
{
	mutex_lock(&df->lock);
	df->suspended = true;
	mutex_unlock(&df->lock);
	cancel_delayed_work_sync(&df->work);
	return (0);
}

int
devfreq_resume_device(struct devfreq *df)
{
	mutex_lock(&df->lock);
	if (df->suspended) {
		df->suspended = false;
		queue_delayed_work(system_wq, &df->work,
		    msecs_to_jiffies(df->profile->polling_ms));
	}
	mutex_unlock(&df->lock);
	return (0);
}

/* A changed minimum takes effect at once, as the next poll. */
static void
msm_fbsd_qos_changed(struct device *dev)
{
	struct devfreq *df;

	mutex_lock(&msm_fbsd_devfreq_list_lock);
	list_for_each_entry(df, &msm_fbsd_devfreqs, link)
		if (df->dev == dev && !READ_ONCE(df->suspended))
			mod_delayed_work(system_wq, &df->work, 0);
	mutex_unlock(&msm_fbsd_devfreq_list_lock);
}

int
dev_pm_qos_add_request(struct device *dev, struct dev_pm_qos_request *req,
    int type, s32 value)
{
	if (type != DEV_PM_QOS_MIN_FREQUENCY)
		return (-EINVAL);
	req->dev = dev;
	req->value = value;
	mutex_lock(&msm_fbsd_devfreq_list_lock);
	req->next = msm_fbsd_qos_reqs;
	msm_fbsd_qos_reqs = req;
	mutex_unlock(&msm_fbsd_devfreq_list_lock);
	msm_fbsd_qos_changed(dev);
	return (0);
}

int
dev_pm_qos_update_request(struct dev_pm_qos_request *req, s32 value)
{
	if (req->dev == NULL)
		return (-EINVAL);
	if (READ_ONCE(req->value) == value)
		return (0);
	WRITE_ONCE(req->value, value);
	msm_fbsd_qos_changed(req->dev);
	return (1);
}

int
dev_pm_qos_remove_request(struct dev_pm_qos_request *req)
{
	struct dev_pm_qos_request **p;
	struct device *dev;

	dev = req->dev;
	if (dev == NULL)
		return (-EINVAL);
	mutex_lock(&msm_fbsd_devfreq_list_lock);
	for (p = &msm_fbsd_qos_reqs; *p != NULL; p = &(*p)->next)
		if (*p == req) {
			*p = req->next;
			break;
		}
	mutex_unlock(&msm_fbsd_devfreq_list_lock);
	req->dev = NULL;
	msm_fbsd_qos_changed(dev);
	return (0);
}

/* What the GPU runs at, and how busy it was, as devfreq last saw. */
static int
msm_fbsd_devfreq_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct devfreq *df;
	u_long v;

	v = 0;
	mutex_lock(&msm_fbsd_devfreq_list_lock);
	df = list_first_entry_or_null(&msm_fbsd_devfreqs, struct devfreq,
	    link);
	if (df != NULL)
		v = arg2 == 0 ? df->previous_freq : df->load;
	mutex_unlock(&msm_fbsd_devfreq_list_lock);
	return (sysctl_handle_long(oidp, &v, 0, req));
}
SYSCTL_PROC(_hw_msm, OID_AUTO, gpu_freq, CTLTYPE_ULONG | CTLFLAG_RD |
    CTLFLAG_MPSAFE, NULL, 0, msm_fbsd_devfreq_sysctl, "LU",
    "The GPU's frequency (Hz), as devfreq last set it");
SYSCTL_PROC(_hw_msm, OID_AUTO, gpu_load, CTLTYPE_ULONG | CTLFLAG_RD |
    CTLFLAG_MPSAFE, NULL, 1, msm_fbsd_devfreq_sysctl, "LU",
    "How busy the GPU was (%) over devfreq's last polling interval");
