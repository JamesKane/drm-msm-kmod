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

/* hrtimer: absolute expiry, and clocks and modes that are not constants. */
#define	HRTIMER_MODE_ABS	0x10
#undef	hrtimer_init
#define	hrtimer_init(timer, clock, mode)	linux_hrtimer_init(timer)
#undef	hrtimer_start
#define	hrtimer_start(timer, time, mode)				\
	linux_hrtimer_start((timer), (mode) == HRTIMER_MODE_ABS ?	\
	    ktime_sub((time), ktime_get()) : (time))

#define	IRQF_TRIGGER_RISING	0x00000001
#define	IRQF_TRIGGER_HIGH	0x00000004

#define	__phys_to_pfn(pa)	((unsigned long)((pa) >> PAGE_SHIFT))

#define	CAP_SYS_RAWIO		17

#ifndef __GFP_DIRECT_RECLAIM
#define	__GFP_DIRECT_RECLAIM	__GFP_WAIT
#endif

#define	LOCK_STATE_NOT_HELD	0

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

static inline int
idr_alloc_u32(struct idr *idr, void *ptr, u32 *nextid, unsigned long max,
    gfp_t gfp)
{
	int id;

	id = idr_alloc(idr, ptr, *nextid, max == UINT_MAX ? 0 : max + 1, gfp);
	if (id < 0)
		return (id);
	*nextid = id;
	return (0);
}

static inline char *
kstrdup_quotable_cmdline(struct task_struct *task, gfp_t gfp)
{
	return (kstrdup(task->comm, gfp));
}

static inline unsigned long long
memparse(const char *s, char **end)
{
	unsigned long long v;

	v = strtouq(s, end, 0);
	switch (**end) {
	case 'g': case 'G':
		v <<= 10;
		/* FALLTHROUGH */
	case 'm': case 'M':
		v <<= 10;
		/* FALLTHROUGH */
	case 'k': case 'K':
		v <<= 10;
		(*end)++;
	}
	return (v);
}

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

void __iomem	*devm_ioremap_resource(struct device *dev,
		    const struct resource *res);

#include <linux/iopoll.h>
#include <linux/sizes.h>
#include <sys/sysctl.h>

/* msm's module parameters live under hw.msm (declared in msm_freebsd.c). */
SYSCTL_DECL(_hw_msm);

#define	readl_poll_timeout(addr, val, cond, sleep_us, timeout_us)	\
	read_poll_timeout(readl, val, cond, sleep_us, timeout_us, false, addr)
#define	readl_poll_timeout_atomic(addr, val, cond, delay_us, timeout_us) \
	read_poll_timeout_atomic(readl, val, cond, delay_us, timeout_us, \
	    false, addr)

/* LinuxKPI's are int expressions, which overflow from 2 GB up. */
#undef	SZ_2G
#undef	SZ_4G
#undef	SZ_8G
#undef	SZ_16G
#undef	SZ_32G
#undef	SZ_64G
#define	SZ_2G		0x0000000080000000ULL
#define	SZ_4G		0x0000000100000000ULL
#define	SZ_8G		0x0000000200000000ULL
#define	SZ_16G		0x0000000400000000ULL
#define	SZ_32G		0x0000000800000000ULL
#define	SZ_64G		0x0000001000000000ULL
#define	SZ_128G		0x0000002000000000ULL
#define	SZ_256G		0x0000004000000000ULL

#define	in_range(val, start, len)					\
	((val) >= (start) && (val) - (start) < (len))

#define	IRQF_NO_AUTOEN		0x00080000

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

#define	SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn)			\
	.suspend = (suspend_fn), .resume = (resume_fn),
#define	RUNTIME_PM_OPS(suspend_fn, resume_fn, idle_fn)			\
	.runtime_suspend = (suspend_fn), .runtime_resume = (resume_fn),	\
	.runtime_idle = (idle_fn),

/* Platform data, which LinuxKPI's struct device has no field for. */
void	*dev_get_platdata(const struct device *dev);
void	msm_freebsd_set_platdata(struct device *dev, void *data);



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

static inline vm_fault_t
vmf_error(int err)
{
	return (err == -ENOMEM ? VM_FAULT_OOM : VM_FAULT_SIGBUS);
}

/* Component matching by device tree node (drm_of.h in Linux). */
#include <linux/component.h>
#include <linux/of.h>

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


/* Interrupts of the glue's platform devices (msm_freebsd.c). */
int	msm_fbsd_request_irq(struct device *dev, unsigned int irq,
	    irq_handler_t handler, unsigned long flags, const char *name,
	    void *arg);
void	msm_fbsd_free_irq(unsigned int irq, void *arg);
void	msm_fbsd_enable_irq(unsigned int irq);
void	msm_fbsd_disable_irq(unsigned int irq);
#define	request_irq(irq, handler, flags, name, arg)			\
	msm_fbsd_request_irq(NULL, (irq), (handler), (flags), (name), (arg))
#define	devm_request_irq(dev, irq, handler, flags, name, arg)		\
	msm_fbsd_request_irq((dev), (irq), (handler), (flags), (name), (arg))
#define	free_irq(irq, arg)	msm_fbsd_free_irq((irq), (arg))
#define	enable_irq(irq)		msm_fbsd_enable_irq(irq)
#define	disable_irq(irq)	msm_fbsd_disable_irq(irq)

/* LinuxKPI's device.h says no device is behind an IOMMU. */
bool	msm_fbsd_device_iommu_mapped(struct device *dev);
#define	device_iommu_mapped(dev)	msm_fbsd_device_iommu_mapped(dev)

#include <linux/pci.h>		/* IORESOURCE_* */

static inline unsigned long
resource_type(const struct resource *r)
{
	return (r->flags & (IORESOURCE_MEM | IORESOURCE_IO | IORESOURCE_IRQ));
}

#endif /* !MSM_FBSD_BUS */
#endif
