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
 * A small device tree for the devices the glue creates: nodes carry the
 * compatible strings, string properties and phandles msm looks up, taken
 * from the SoC description in msm_freebsd_soc.c.  Replaces LinuxKPI's
 * placeholder.
 */
#ifndef _MSM_FREEBSD_LINUX_OF_H_
#define	_MSM_FREEBSD_LINUX_OF_H_

#include <linux/types.h>
#include <linux/kobject.h>
#include <linux/ioport.h>
#include <linux/mod_devicetable.h>

struct device;
struct platform_device;

struct of_prop {
	const char		*name;
	const char		*str;		/* string property */
	const char		*phandle;	/* name of the node referred to */
	const u32		*cells;		/* u32 array property */
	u_int			ncells;
};

struct device_node {
	const char		*name;
	const char *const	*compatible;	/* NULL terminated */
	const char		*parent_name;
	const struct of_prop	*props;		/* NULL name terminated */
	struct resource		reg;		/* for of_address_to_resource */
	bool			disabled;
	struct platform_device	*pdev;		/* set by the glue */
};

#ifndef _MSM_FREEBSD_OF_DEVICE_ID
#define	_MSM_FREEBSD_OF_DEVICE_ID
struct of_device_id {
	char		name[32];
	char		type[32];
	char		compatible[128];
	const void	*data;
};
#endif

struct device_node *dev_of_node(struct device *dev);
void	of_node_put(struct device_node *np);
struct device_node *of_node_get(struct device_node *np);
bool	of_device_is_compatible(const struct device_node *np,
	    const char *compat);
bool	of_device_is_available(const struct device_node *np);
bool	of_machine_is_compatible(const char *compat);
int	of_device_compatible_match(const struct device_node *np,
	    const char *const *compat);
struct device_node *of_parse_phandle(const struct device_node *np,
	    const char *name, int index);
struct device_node *of_get_child_by_name(const struct device_node *np,
	    const char *name);
struct device_node *of_find_matching_node(struct device_node *from,
	    const struct of_device_id *matches);
const struct of_device_id *of_match_node(const struct of_device_id *matches,
	    const struct device_node *np);
int	of_property_read_string_index(const struct device_node *np,
	    const char *name, int index, const char **out);
int	of_property_read_u32_index(const struct device_node *np,
	    const char *name, u32 index, u32 *out);
bool	of_property_read_bool(const struct device_node *np, const char *name);
int	of_address_to_resource(struct device_node *np, int index,
	    struct resource *r);
struct platform_device *of_find_device_by_node(struct device_node *np);
int	of_dma_configure(struct device *dev, struct device_node *np,
	    bool force_dma);

static inline int
of_property_read_string(const struct device_node *np, const char *name,
    const char **out)
{
	return (of_property_read_string_index(np, name, 0, out));
}

static inline int
of_property_read_u32(const struct device_node *np, const char *name, u32 *out)
{
	return (of_property_read_u32_index(np, name, 0, out));
}

static inline const char *
of_node_full_name(const struct device_node *np)
{
	return (np != NULL ? np->name : "<no-node>");
}

#define	for_each_child_of_node(parent, child)	\
	for ((child) = NULL; (child) != NULL;)

/* Graph links are display only. */
struct of_endpoint {
	unsigned int		port;
	unsigned int		id;
	const struct device_node *local_node;
};

static inline struct device_node *
of_graph_get_remote_port_parent(const struct device_node *np __unused)
{
	return (NULL);
}

static inline int
of_graph_parse_endpoint(const struct device_node *np __unused,
    struct of_endpoint *ep __unused)
{
	return (-EINVAL);
}

#endif
