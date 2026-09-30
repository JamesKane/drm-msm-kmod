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
 * Small Linux interfaces msm uses that LinuxKPI lacks.  Included before
 * everything else in every msm source file (see msm/Makefile).  Generic
 * ones are candidates for LinuxKPI.
 */
#ifndef _MSM_FREEBSD_COMPAT_H_
#define	_MSM_FREEBSD_COMPAT_H_
#ifndef MSM_FBSD_BUS	/* msm_freebsd_bus.c uses FreeBSD's struct resource */

#include <linux/types.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/hrtimer.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>

struct device;
struct notifier_block;


/* Fault injection: never inject. */
struct fault_attr {
	int	unused;
};
#define	DECLARE_FAULT_ATTR(name)	struct fault_attr name = { 0 }
#define	should_fail(attr, size)		((void)(attr), (void)(size), false)
#define	fault_create_debugfs_attr(name, parent, attr)			\
	((void)(parent), (void)(attr), NULL)

/* vmap purge notifiers: FreeBSD's KVA is not purged lazily. */
#define	register_vmap_purge_notifier(nb)	((void)(nb), 0)
#define	unregister_vmap_purge_notifier(nb)	((void)(nb), 0)

/* PM QoS: no frequency constraints (there is no devfreq). */
#define	DEV_PM_QOS_MIN_FREQUENCY	1
#define	PM_QOS_MIN_FREQUENCY_DEFAULT_VALUE 0
struct dev_pm_qos_request {
	int	unused;
};
#define	dev_pm_qos_add_request(dev, req, type, value)	((void)(req), 0)
#define	dev_pm_qos_update_request(req, value)		((void)(req), 0)
#define	dev_pm_qos_remove_request(req)			((void)(req), 0)
#define	DEVFREQ_TIMER_DELAYED				0

/* No regulators on the supported boards; msm treats them as optional. */
struct regulator;
#define	devm_regulator_get(dev, id)	((struct regulator *)ERR_PTR(-ENODEV))
#define	regulator_enable(r)		((void)(r), 0)
#define	regulator_disable(r)		((void)(r), 0)


#include <linux/iopoll.h>
#include <linux/sizes.h>
#include <sys/sysctl.h>

/* msm's module parameters live under hw.msm (declared in msm_freebsd_bus.c). */
SYSCTL_DECL(_hw_msm);


/* Device links: the glue powers the GMU and GPU in the right order. */
#define	DL_FLAG_STATELESS	0x0001
#define	DL_FLAG_PM_RUNTIME	0x0004
struct device_link {
	int	unused;
};
static inline struct device_link *
device_link_add(struct device *consumer __unused,
    struct device *supplier __unused, u32 flags __unused)
{
	static struct device_link link;

	return (&link);
}
#define	device_link_del(link)	((void)(link))

/* Fuses (speed bins): none read; msm then uses the default bin. */
#define	nvmem_cell_read_variable_le_u32(dev, name, val)			\
	((void)(dev), (void)(name), (void)(val), -ENOENT)



/* GEM faults, as TTM does them: insert under the VM object's lock. */
#include <linux/mm.h>
#include <vm/vm.h>
#include <vm/vm_object.h>

static inline vm_fault_t
vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr,
    unsigned long pfn)
{
	vm_fault_t ret;

	VM_OBJECT_WLOCK(vma->vm_obj);
	ret = lkpi_vmf_insert_pfn_prot_locked(vma, addr, pfn,
	    vma->vm_page_prot);
	VM_OBJECT_WUNLOCK(vma->vm_obj);
	return (ret);
}

/* Component matching by device tree node (drm_of.h in Linux). */
#include <linux/component.h>
#include <linux/of.h>

int	component_compare_of(struct device *dev, void *data);
void	component_release_of(struct device *dev, void *data);

static inline void
drm_of_component_match_add(struct device *master,
    struct component_match **matchptr,
    int (*compare)(struct device *, void *), struct device_node *node)
{
	of_node_get(node);
	component_match_add_release(master, matchptr, component_release_of,
	    compare, node);
}

/* Display graphs are not described; there are no endpoints. */
#define	for_each_endpoint_of_node(parent, child)			\
	for ((child) = NULL; (child) != NULL; )



/* LinuxKPI's device.h says no device is behind an IOMMU. */
bool	msm_fbsd_device_iommu_mapped(struct device *dev);
#define	device_iommu_mapped(dev)	msm_fbsd_device_iommu_mapped(dev)

#include <linux/pci.h>		/* IORESOURCE_* */

/* Not in LinuxKPI, whose pci.h sees FreeBSD's struct resource. */
static inline unsigned long
resource_type(const struct resource *r)
{
	return (r->flags & (IORESOURCE_MEM | IORESOURCE_IO | IORESOURCE_IRQ));
}

/* drm-kmod's linux/fb.h defines these, which the register headers use. */
#include <linux/fb.h>
#undef ROP_COPY
#undef ROP_XOR

#endif /* !MSM_FBSD_BUS */
#endif
