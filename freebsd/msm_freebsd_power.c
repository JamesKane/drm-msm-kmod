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
 * Power domains, clocks and OPP tables for the glue's platform devices.
 * Runtime PM is LinuxKPI's.  The GMU's CX domain is powered for as long as
 * the glue is attached, so its domain device only counts, as does the GX
 * domain, which the GMU firmware powers.  Clocks are placeholders: CX's are
 * on with the domain, and the GMU sets the GPU core clock.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include "msm_freebsd.h"

/* Power domains: devices standing for the GMU's CX and GX domains. */

static void
msm_fbsd_pd_release(struct device *pd)
{
	kfree(pd);
}

/* As in Linux, a virtual device per domain, gone at detach. */
struct device *
dev_pm_domain_attach_by_name(struct device *dev, const char *name)
{
	struct device *pd;

	if (strcmp(name, "cx") != 0 && strcmp(name, "gx") != 0)
		return (ERR_PTR(-ENODEV));
	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (pd == NULL)
		return (ERR_PTR(-ENOMEM));
	lkpi_device_init(pd, dev, dev->bsddev);
	kobject_init(&pd->kobj, &linux_dev_ktype);
	kobject_set_name(&pd->kobj, "%s-pd", name);
	pd->release = msm_fbsd_pd_release;
	pm_runtime_enable(pd);
	return (pd);
}

void
dev_pm_domain_detach(struct device *pd, bool power_off __unused)
{
	if (pd == NULL || IS_ERR(pd))
		return;
	pm_runtime_disable(pd);
	put_device(pd);
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

/*
 * OPP tables: the SoC description's, whose entries are the struct
 * dev_pm_opp that Linux keeps opaque.
 */

static const struct dev_pm_opp *
msm_fbsd_opps(struct device *dev)
{
	const struct msm_fbsd_pdev_desc *desc = msm_fbsd_pdev_desc(dev);

	return (desc != NULL ? desc->opps : NULL);
}

int
devm_pm_opp_of_add_table(struct device *dev)
{
	return (msm_fbsd_opps(dev) != NULL ? 0 : -ENODEV);
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
	const struct dev_pm_opp *o = msm_fbsd_opps(dev);
	int n;

	if (o == NULL)
		return (-ENODEV);
	for (n = 0; o[n].hz != 0; n++)
		;
	return (n);
}

struct dev_pm_opp *
dev_pm_opp_find_freq_exact(struct device *dev, unsigned long freq,
    bool available __unused)
{
	const struct dev_pm_opp *o = msm_fbsd_opps(dev);

	for (; o != NULL && o->hz != 0; o++)
		if (o->hz == freq)
			return (__DECONST(struct dev_pm_opp *, o));
	return (ERR_PTR(-ERANGE));
}

/* The OPP nearest *freq at or above it (ceil) or at or below it. */
static struct dev_pm_opp *
msm_fbsd_opp_find(struct device *dev, unsigned long *freq, bool ceil)
{
	const struct dev_pm_opp *o = msm_fbsd_opps(dev), *best = NULL;

	for (; o != NULL && o->hz != 0; o++)
		if ((ceil ? o->hz >= *freq : o->hz <= *freq) &&
		    (best == NULL || (ceil ? o->hz < best->hz :
		    o->hz > best->hz)))
			best = o;
	if (best == NULL)
		return (ERR_PTR(-ERANGE));
	*freq = best->hz;
	return (__DECONST(struct dev_pm_opp *, best));
}

struct dev_pm_opp *
dev_pm_opp_find_freq_ceil(struct device *dev, unsigned long *freq)
{
	return (msm_fbsd_opp_find(dev, freq, true));
}

struct dev_pm_opp *
dev_pm_opp_find_freq_floor(struct device *dev, unsigned long *freq)
{
	return (msm_fbsd_opp_find(dev, freq, false));
}

unsigned long
dev_pm_opp_get_freq(struct dev_pm_opp *opp)
{
	return (opp->hz);
}

unsigned int
dev_pm_opp_get_level(struct dev_pm_opp *opp)
{
	return (opp->level);
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
