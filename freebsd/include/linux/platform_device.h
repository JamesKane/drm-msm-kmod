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
 * Platform devices and drivers, for the devices the glue creates from the
 * ACPI GPU device (msm_freebsd.c).  Replaces LinuxKPI's stubs.
 */
#ifndef _MSM_FREEBSD_LINUX_PLATFORM_DEVICE_H_
#define	_MSM_FREEBSD_LINUX_PLATFORM_DEVICE_H_

#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/of.h>

struct platform_device {
	const char		*name;
	int			id;
	bool			id_auto;
	struct device		dev;
	u32			num_resources;
	struct resource		*resource;
	const struct platform_device_id *id_entry;
	/* FreeBSD glue */
	struct platform_driver	*bound;
};

#define	to_platform_device(d)	container_of((d), struct platform_device, dev)
#define	dev_is_platform(d)	(true)

struct platform_device_info {
	struct device		*parent;
	const char		*name;
	int			id;
	const struct resource	*res;
	unsigned int		num_res;
	const void		*data;
	size_t			size_data;
	u64			dma_mask;
};

struct platform_driver {
	int	(*probe)(struct platform_device *);
	void	(*remove)(struct platform_device *);
	void	(*shutdown)(struct platform_device *);
	struct device_driver driver;
};

int	platform_driver_register(struct platform_driver *pdrv);
void	platform_driver_unregister(struct platform_driver *pdrv);
struct platform_device *platform_device_register_full(
	    const struct platform_device_info *info);
struct resource *platform_get_resource(struct platform_device *pdev,
	    unsigned int type, unsigned int num);
struct resource *platform_get_resource_byname(struct platform_device *pdev,
	    unsigned int type, const char *name);
int	platform_get_irq(struct platform_device *pdev, unsigned int num);
int	platform_get_irq_byname(struct platform_device *pdev,
	    const char *name);

static inline void *
platform_get_drvdata(const struct platform_device *pdev)
{
	return (dev_get_drvdata(&pdev->dev));
}

static inline void
platform_set_drvdata(struct platform_device *pdev, void *data)
{
	dev_set_drvdata(&pdev->dev, data);
}

#endif
