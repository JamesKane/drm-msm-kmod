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
 * The component framework, as msm uses it: one master (the msm DRM device)
 * binds its components (the GPU) once they are all registered
 * (msm_freebsd_component.c).
 */
#ifndef _MSM_FREEBSD_LINUX_COMPONENT_H_
#define	_MSM_FREEBSD_LINUX_COMPONENT_H_

#include <linux/device.h>

struct component_match;

struct component_ops {
	int	(*bind)(struct device *comp, struct device *master, void *data);
	void	(*unbind)(struct device *comp, struct device *master,
		    void *data);
};

struct component_master_ops {
	int	(*bind)(struct device *master);
	void	(*unbind)(struct device *master);
};

int	component_add(struct device *dev, const struct component_ops *ops);
void	component_del(struct device *dev, const struct component_ops *ops);
int	component_bind_all(struct device *master, void *data);
void	component_unbind_all(struct device *master, void *data);
void	component_match_add_release(struct device *master,
	    struct component_match **matchptr,
	    void (*release)(struct device *, void *),
	    int (*compare)(struct device *, void *), void *compare_data);
int	component_master_add_with_match(struct device *master,
	    const struct component_master_ops *ops,
	    struct component_match *match);
void	component_master_del(struct device *master,
	    const struct component_master_ops *ops);
int	component_compare_of(struct device *dev, void *data);
void	component_release_of(struct device *dev, void *data);

static inline void
component_match_add(struct device *master, struct component_match **matchptr,
    int (*compare)(struct device *, void *), void *compare_data)
{
	component_match_add_release(master, matchptr, NULL, compare,
	    compare_data);
}

#endif
