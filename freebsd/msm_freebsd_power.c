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
 * Runtime PM, power domains, clocks and OPP tables for the glue's platform
 * devices.
 *
 * Runtime PM is synchronous and counted: the first get resumes a device
 * through its driver's runtime_resume, the last put suspends it.  The GMU's
 * CX domain is powered for as long as the glue is attached, so its domain
 * device only counts, as does the GX domain, which the GMU firmware powers.
 * Clocks are placeholders: CX's are on with the domain, and the GMU sets the
 * GPU core clock.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>

#include "msm_freebsd.h"

/* Runtime PM */

static struct msm_fbsd_rpm {
	struct device	*dev;
	int		usage;
	bool		enabled;
	bool		active;
} msm_fbsd_rpm[8];

static DEFINE_MUTEX(msm_fbsd_rpm_lock);

static struct msm_fbsd_rpm *
msm_fbsd_rpm_get(struct device *dev)
{
	struct msm_fbsd_rpm *free = NULL;
	int i;

	for (i = 0; i < nitems(msm_fbsd_rpm); i++) {
		if (msm_fbsd_rpm[i].dev == dev)
			return (&msm_fbsd_rpm[i]);
		if (free == NULL && msm_fbsd_rpm[i].dev == NULL)
			free = &msm_fbsd_rpm[i];
	}
	if (free != NULL) {
		free->dev = dev;
		free->usage = 0;
		free->enabled = false;
		free->active = false;
	}
	return (free);
}

static const struct dev_pm_ops *
msm_fbsd_pm_ops(struct device *dev)
{
	return (dev->driver != NULL ? dev->driver->pm : NULL);
}

static int
msm_fbsd_rpm_resume(struct device *dev, struct msm_fbsd_rpm *r)
{
	const struct dev_pm_ops *ops = msm_fbsd_pm_ops(dev);
	int error;

	if (r->active)
		return (0);
	if (r->enabled && ops != NULL && ops->runtime_resume != NULL) {
		error = ops->runtime_resume(dev);
		if (error != 0)
			return (error);
	}
	r->active = true;
	return (0);
}

static void
msm_fbsd_rpm_suspend(struct device *dev, struct msm_fbsd_rpm *r)
{
	const struct dev_pm_ops *ops = msm_fbsd_pm_ops(dev);

	if (!r->active)
		return;
	if (r->enabled && ops != NULL && ops->runtime_suspend != NULL)
		(void)ops->runtime_suspend(dev);
	r->active = false;
}

int
pm_runtime_get_sync(struct device *dev)
{
	struct msm_fbsd_rpm *r;
	int error;

	mutex_lock(&msm_fbsd_rpm_lock);
	r = msm_fbsd_rpm_get(dev);
	if (r == NULL) {
		mutex_unlock(&msm_fbsd_rpm_lock);
		return (-ENOMEM);
	}
	r->usage++;
	error = r->usage == 1 ? msm_fbsd_rpm_resume(dev, r) : 0;
	mutex_unlock(&msm_fbsd_rpm_lock);
	return (error);		/* the count stays up, as in Linux */
}

int
pm_runtime_put_sync(struct device *dev)
{
	struct msm_fbsd_rpm *r;

	mutex_lock(&msm_fbsd_rpm_lock);
	r = msm_fbsd_rpm_get(dev);
	if (r != NULL && r->usage > 0 && --r->usage == 0)
		msm_fbsd_rpm_suspend(dev, r);
	mutex_unlock(&msm_fbsd_rpm_lock);
	return (0);
}

int
pm_runtime_get_if_in_use(struct device *dev)
{
	struct msm_fbsd_rpm *r;
	int ret;

	mutex_lock(&msm_fbsd_rpm_lock);
	r = msm_fbsd_rpm_get(dev);
	ret = r != NULL && r->active && r->usage > 0;
	if (ret)
		r->usage++;
	mutex_unlock(&msm_fbsd_rpm_lock);
	return (ret);
}

bool
pm_runtime_active(struct device *dev)
{
	struct msm_fbsd_rpm *r;
	bool active;

	mutex_lock(&msm_fbsd_rpm_lock);
	r = msm_fbsd_rpm_get(dev);
	active = r != NULL && r->active;
	mutex_unlock(&msm_fbsd_rpm_lock);
	return (active);
}

void
pm_runtime_enable(struct device *dev)
{
	struct msm_fbsd_rpm *r;

	mutex_lock(&msm_fbsd_rpm_lock);
	if ((r = msm_fbsd_rpm_get(dev)) != NULL)
		r->enabled = true;
	mutex_unlock(&msm_fbsd_rpm_lock);
}

void
pm_runtime_disable(struct device *dev)
{
	struct msm_fbsd_rpm *r;

	mutex_lock(&msm_fbsd_rpm_lock);
	if ((r = msm_fbsd_rpm_get(dev)) != NULL)
		r->enabled = false;
	mutex_unlock(&msm_fbsd_rpm_lock);
}

bool
pm_runtime_enabled(struct device *dev)
{
	struct msm_fbsd_rpm *r;
	bool enabled;

	mutex_lock(&msm_fbsd_rpm_lock);
	r = msm_fbsd_rpm_get(dev);
	enabled = r != NULL && r->enabled;
	mutex_unlock(&msm_fbsd_rpm_lock);
	return (enabled);
}

int
pm_runtime_force_suspend(struct device *dev)
{
	struct msm_fbsd_rpm *r;

	mutex_lock(&msm_fbsd_rpm_lock);
	if ((r = msm_fbsd_rpm_get(dev)) != NULL)
		msm_fbsd_rpm_suspend(dev, r);
	mutex_unlock(&msm_fbsd_rpm_lock);
	return (0);
}

int
pm_runtime_force_resume(struct device *dev)
{
	struct msm_fbsd_rpm *r;
	int error = 0;

	mutex_lock(&msm_fbsd_rpm_lock);
	if ((r = msm_fbsd_rpm_get(dev)) != NULL && r->usage > 0)
		error = msm_fbsd_rpm_resume(dev, r);
	mutex_unlock(&msm_fbsd_rpm_lock);
	return (error);
}

/* Power domains: devices standing for the GMU's CX and GX domains. */

static struct device msm_fbsd_cxpd, msm_fbsd_gxpd;

struct device *
dev_pm_domain_attach_by_name(struct device *dev, const char *name)
{
	struct device *pd;

	if (strcmp(name, "cx") == 0)
		pd = &msm_fbsd_cxpd;
	else if (strcmp(name, "gx") == 0)
		pd = &msm_fbsd_gxpd;
	else
		return (ERR_PTR(-ENODEV));
	pd->bsddev = dev->bsddev;
	pm_runtime_enable(pd);
	return (pd);
}

void
dev_pm_domain_detach(struct device *pd, bool power_off __unused)
{
	if (pd != NULL && !IS_ERR(pd))
		pm_runtime_disable(pd);
}

/* Clocks */

struct clk *
devm_clk_get(struct device *dev __unused, const char *id __unused)
{
	return (ERR_PTR(-ENOENT));
}

int
devm_clk_bulk_get_all(struct device *dev __unused,
    struct clk_bulk_data **clks)
{
	*clks = NULL;
	return (0);
}

int
clk_prepare_enable(struct clk *clk __unused)
{
	return (0);
}

void
clk_disable_unprepare(struct clk *clk __unused)
{
}

int
clk_set_rate(struct clk *clk __unused, unsigned long rate __unused)
{
	return (0);
}

unsigned long
clk_get_rate(struct clk *clk __unused)
{
	return (0);
}

/* OPP tables, from the SoC description. */

struct dev_pm_opp {
	const struct msm_fbsd_opp *o;
};

#define	MSM_FBSD_MAX_OPPS	16

static struct msm_fbsd_opps {
	struct device		*dev;
	struct dev_pm_opp	opp[MSM_FBSD_MAX_OPPS];
	int			n;
} msm_fbsd_opps[MSM_FBSD_MAX_PDEVS];

static struct msm_fbsd_opps *
msm_fbsd_opps_of(struct device *dev)
{
	int i;

	for (i = 0; i < nitems(msm_fbsd_opps); i++)
		if (msm_fbsd_opps[i].dev == dev)
			return (&msm_fbsd_opps[i]);
	return (NULL);
}

int
devm_pm_opp_of_add_table(struct device *dev)
{
	const struct msm_fbsd_pdev_desc *desc;
	struct msm_fbsd_opps *t;
	int i;

	desc = msm_fbsd_pdev_desc(dev);
	if (desc == NULL || desc->opps == NULL)
		return (-ENODEV);
	if (msm_fbsd_opps_of(dev) != NULL)
		return (0);
	for (i = 0; i < nitems(msm_fbsd_opps); i++)
		if (msm_fbsd_opps[i].dev == NULL)
			break;
	if (i == nitems(msm_fbsd_opps))
		return (-ENOSPC);
	t = &msm_fbsd_opps[i];
	t->dev = dev;
	for (t->n = 0; t->n < MSM_FBSD_MAX_OPPS && desc->opps[t->n].hz != 0;
	    t->n++)
		t->opp[t->n].o = &desc->opps[t->n];
	return (0);
}

int
devm_pm_opp_set_clkname(struct device *dev __unused, const char *name __unused)
{
	return (0);
}

int
devm_pm_opp_set_supported_hw(struct device *dev __unused,
    const u32 *versions __unused, unsigned int count __unused)
{
	return (0);
}

int
dev_pm_opp_set_config(struct device *dev __unused,
    struct dev_pm_opp_config *config __unused)
{
	return (0);
}

/* Used only when there is no table; ours always has one. */
int
dev_pm_opp_add(struct device *dev __unused, unsigned long freq __unused,
    unsigned long u_volt __unused)
{
	return (-EOPNOTSUPP);
}

int
dev_pm_opp_get_opp_count(struct device *dev)
{
	struct msm_fbsd_opps *t = msm_fbsd_opps_of(dev);

	return (t != NULL ? t->n : -ENODEV);
}

struct dev_pm_opp *
dev_pm_opp_find_freq_exact(struct device *dev, unsigned long freq,
    bool available __unused)
{
	struct msm_fbsd_opps *t = msm_fbsd_opps_of(dev);
	int i;

	for (i = 0; t != NULL && i < t->n; i++)
		if (t->opp[i].o->hz == freq)
			return (&t->opp[i]);
	return (ERR_PTR(-ERANGE));
}

/* The lowest OPP at or above *freq. */
struct dev_pm_opp *
dev_pm_opp_find_freq_ceil(struct device *dev, unsigned long *freq)
{
	struct msm_fbsd_opps *t = msm_fbsd_opps_of(dev);
	struct dev_pm_opp *best = NULL;
	int i;

	for (i = 0; t != NULL && i < t->n; i++)
		if (t->opp[i].o->hz >= *freq &&
		    (best == NULL || t->opp[i].o->hz < best->o->hz))
			best = &t->opp[i];
	if (best == NULL)
		return (ERR_PTR(-ERANGE));
	*freq = best->o->hz;
	return (best);
}

/* The highest OPP at or below *freq. */
struct dev_pm_opp *
dev_pm_opp_find_freq_floor(struct device *dev, unsigned long *freq)
{
	struct msm_fbsd_opps *t = msm_fbsd_opps_of(dev);
	struct dev_pm_opp *best = NULL;
	int i;

	for (i = 0; t != NULL && i < t->n; i++)
		if (t->opp[i].o->hz <= *freq &&
		    (best == NULL || t->opp[i].o->hz > best->o->hz))
			best = &t->opp[i];
	if (best == NULL)
		return (ERR_PTR(-ERANGE));
	*freq = best->o->hz;
	return (best);
}

unsigned long
dev_pm_opp_get_freq(struct dev_pm_opp *opp)
{
	return (opp->o->hz);
}

unsigned int
dev_pm_opp_get_level(struct dev_pm_opp *opp)
{
	return (opp->o->level);
}

void
dev_pm_opp_put(struct dev_pm_opp *opp __unused)
{
}

/*
 * Setting an OPP would set clocks and bandwidth votes.  The GMU clock stays
 * at 200 MHz, and the GMU sets the GPU's clock and votes its bandwidth.
 */
int
dev_pm_opp_set_opp(struct device *dev __unused, struct dev_pm_opp *opp __unused)
{
	return (0);
}

int
dev_pm_opp_set_rate(struct device *dev __unused, unsigned long freq __unused)
{
	return (0);
}

int
dev_pm_opp_of_find_icc_paths(struct device *dev __unused, void *table __unused)
{
	return (0);
}
