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
 * Device tree lookups, as msm uses them, for the devices msm_freebsd.c
 * creates from the ACPI GPU device.  The devices themselves are on
 * LinuxKPI's platform bus, and components are LinuxKPI's.
 */

#include <linux/device.h>
#include <linux/component.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "msm_freebsd.h"


/* Device tree */

struct device_node *
of_node_get(struct device_node *np)
{
	return (np);		/* nodes are static */
}

void
of_node_put(struct device_node *np __unused)
{
}

bool
of_device_is_compatible(const struct device_node *np, const char *compat)
{
	const char *const *c;

	if (np == NULL || np->compatible == NULL)
		return (false);
	for (c = np->compatible; *c != NULL; c++)
		if (strcmp(*c, compat) == 0)
			return (true);
	return (false);
}

int
of_device_compatible_match(const struct device_node *np,
    const char *const *compat)
{
	for (; *compat != NULL; compat++)
		if (of_device_is_compatible(np, *compat))
			return (1);
	return (0);
}

bool
of_device_is_available(const struct device_node *np)
{
	return (np != NULL && !np->disabled);
}

bool
of_machine_is_compatible(const char *compat)
{
	return (of_device_is_compatible(msm_fbsd_soc->machine, compat));
}

static const struct of_prop *
of_find_prop(const struct device_node *np, const char *name)
{
	const struct of_prop *p;

	if (np == NULL || np->props == NULL)
		return (NULL);
	for (p = np->props; p->name != NULL; p++)
		if (strcmp(p->name, name) == 0)
			return (p);
	return (NULL);
}

static struct device_node *
of_find_node_by_name(const char *name)
{
	struct device_node *np;

	for (np = msm_fbsd_soc->nodes; np->name != NULL; np++)
		if (strcmp(np->name, name) == 0)
			return (np);
	return (NULL);
}

struct device_node *
of_parse_phandle(const struct device_node *np, const char *name, int index)
{
	const struct of_prop *p;

	p = of_find_prop(np, name);
	if (p == NULL || p->phandle == NULL || index != 0)
		return (NULL);
	return (of_find_node_by_name(p->phandle));
}

struct device_node *
of_get_child_by_name(const struct device_node *np, const char *name)
{
	struct device_node *c;

	if (np == NULL)
		return (NULL);
	for (c = msm_fbsd_soc->nodes; c->name != NULL; c++)
		if (c->parent_name != NULL &&
		    strcmp(c->parent_name, np->name) == 0 &&
		    strcmp(c->name, name) == 0)
			return (c);
	return (NULL);
}

const struct of_device_id *
of_match_node(const struct of_device_id *matches,
    const struct device_node *np)
{
	for (; matches != NULL && matches->compatible[0] != '\0'; matches++)
		if (of_device_is_compatible(np, matches->compatible))
			return (matches);
	return (NULL);
}

struct device_node *
of_find_matching_node(struct device_node *from,
    const struct of_device_id *matches)
{
	struct device_node *np;

	np = from != NULL ? from + 1 : msm_fbsd_soc->nodes;
	for (; np->name != NULL; np++)
		if (of_device_is_available(np) &&
		    of_match_node(matches, np) != NULL)
			return (np);
	return (NULL);
}

int
of_property_read_string_index(const struct device_node *np, const char *name,
    int index, const char **out)
{
	const struct of_prop *p;
	int i;

	if (np == NULL)
		return (-EINVAL);
	if (strcmp(name, "compatible") == 0) {
		for (i = 0; np->compatible != NULL &&
		    np->compatible[i] != NULL; i++)
			if (i == index) {
				*out = np->compatible[i];
				return (0);
			}
		return (-ENODATA);
	}
	p = of_find_prop(np, name);
	if (p == NULL || p->str == NULL)
		return (-EINVAL);
	if (index != 0)
		return (-ENODATA);
	*out = p->str;
	return (0);
}

int
of_property_read_u32_index(const struct device_node *np, const char *name,
    u32 index, u32 *out)
{
	const struct of_prop *p;

	p = of_find_prop(np, name);
	if (p == NULL || p->cells == NULL)
		return (-EINVAL);
	if (index >= p->ncells)
		return (-EOVERFLOW);
	*out = p->cells[index];
	return (0);
}

bool
of_property_read_bool(const struct device_node *np, const char *name)
{
	return (of_find_prop(np, name) != NULL);
}

int
of_address_to_resource(struct device_node *np, int index, struct resource *r)
{
	if (np == NULL || index != 0 || np->reg.end == 0)
		return (-EINVAL);
	*r = np->reg;
	return (0);
}

struct platform_device *
of_find_device_by_node(struct device_node *np)
{
	/* As in Linux, with a reference the caller drops with put_device(). */
	if (np == NULL || np->pdev == NULL)
		return (NULL);
	get_device(&np->pdev->dev);
	return (np->pdev);
}

int
of_dma_configure(struct device *dev __unused, struct device_node *np __unused,
    bool force_dma __unused)
{
	return (0);
}

/* Component matching by device tree node, for drm_of_component_match_add(). */

int
component_compare_of(struct device *dev, void *data)
{
	return (dev_of_node(dev) == data);
}

void
component_release_of(struct device *dev __unused, void *data)
{
	of_node_put(data);
}
