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
 * msmfb: a KMS driver for the display pipeline the boot firmware left
 * running.  The firmware scans its framebuffer out through one DPU source
 * pipe, layer mixer, control path and interface to a DisplayPort output; the
 * link, the PHY and the clocks stay as it set them, so the mode cannot
 * change.  A flip points the source pipe at the client's buffer and flushes
 * it, which takes effect at the next vsync; the interface's vsync interrupt
 * drives vblank.  With no client, the pipe shows the firmware framebuffer and
 * the console.
 *
 * The DisplayPort controller's AUX channel reads the sink's capabilities and
 * the monitor's EDID, which the connector reports.  Its transfers are polled:
 * the DP controller's interrupts stay masked, as the firmware left them.
 *
 * The firmware leaves the display's SMMU streams in bypass, so the pipe
 * fetches physical addresses, through 32-bit registers: buffers are
 * physically contiguous and below 4 GB.  They are write-combining, as the
 * display does not snoop the CPU's caches.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/fbio.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include <dev/vt/vt.h>

#include <linux/device.h>
#include <linux/fb.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/display/drm_dp.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_prime.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "msm_freebsd.h"

#define	MSMFB_CPP		4

/* DPU registers, relative to the MDP block. */
#define	MDP_INTR_EN		0x010
#define	MDP_INTR_STATUS		0x014
#define	MDP_INTR_CLEAR		0x018
#define	SSPP_SRC_SIZE		0x000
#define	SSPP_SRC0_ADDR		0x014
#define	SSPP_SRC_YSTRIDE0	0x024
#define	CTL_FLUSH		0x018

/* DisplayPort controller registers, relative to the controller. */
#define	DP_INTR_STATUS		0x020		/* AHB block */
#define	DP_AUX_DATA		0x234		/* AUX block from here */
#define	 DP_AUX_DATA_READ	(1u << 0)
#define	 DP_AUX_DATA_INDEX_WRITE (1u << 31)
#define	DP_AUX_TRANS_CTRL	0x238
#define	 DP_AUX_TRANS_I2C	(1u << 8)
#define	 DP_AUX_TRANS_GO	(1u << 9)
#define	 DP_AUX_TRANS_NO_SEND_ADDR (1u << 10)
#define	 DP_AUX_TRANS_NO_SEND_STOP (1u << 11)
#define	DP_PHY_AUX_INTR_CLEAR	0x24c
#define	DP_PHY_AUX_INTR_STATUS	0x2bc
/*
 * DP_INTR_STATUS has a status bit per event, and above each its
 * acknowledge and mask bits.
 */
#define	DP_INTR_AUX_DONE	(1u << 3)
#define	DP_INTR_AUX_FAILED	((1u << 6) | (1u << 9) | (1u << 12) | \
				 (1u << 15) | (1u << 18) | (1u << 21) | \
				 (1u << 27))
#define	DP_AUX_TIMEOUT_US	250000
#define	DP_EDID_ADDR		0x50

extern struct vt_device *main_vd;

struct msmfb {
	struct drm_device		drm;
	const struct msm_fbsd_disp	*disp;
	void __iomem			*mdp;
	void __iomem			*dp;
	const struct drm_edid		*edid;		/* or NULL */
	u32				console_addr;	/* the firmware's scanout */
	u32				console_stride;
	u_int				width, height;
	struct drm_display_mode		mode;
	struct drm_plane		plane;
	struct drm_crtc			crtc;
	struct drm_encoder		encoder;
	struct drm_connector		connector;
	spinlock_t			lock;		/* registers, below */
	struct drm_pending_vblank_event	*event;		/* of a latching flip */
	bool				vt_frozen;	/* by master_set */
};

#define	to_msmfb(d)	container_of(d, struct msmfb, drm)

/* A dumb buffer: 2^order physically contiguous pages below 4 GB. */
struct msmfb_bo {
	struct drm_gem_object	base;
	struct page		*page;
	u_int			order;
	struct page		**pages;	/* each of them, for PRIME and vmap */
	u_int			npages;
	void			*vaddr;		/* write-combining */
};

#define	to_msmfb_bo(obj)	container_of(obj, struct msmfb_bo, base)

static const uint32_t msmfb_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const uint64_t msmfb_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static inline u32
msmfb_read(struct msmfb *fb, u_int off)
{
	return (readl(fb->mdp + off));
}

static inline void
msmfb_write(struct msmfb *fb, u_int off, u32 val)
{
	writel(val, fb->mdp + off);
}

/*
 * Point the source pipe at a buffer and flush it; the pipe latches the
 * address at the next vsync.
 */
static void
msmfb_scanout(struct msmfb *fb, u32 addr, u32 stride)
{
	const struct msm_fbsd_disp *d = fb->disp;
	u32 v;

	msmfb_write(fb, d->sspp + SSPP_SRC0_ADDR, addr);
	v = msmfb_read(fb, d->sspp + SSPP_SRC_YSTRIDE0);
	msmfb_write(fb, d->sspp + SSPP_SRC_YSTRIDE0, (v & 0xffff0000) |
	    (stride & 0xffff));
	msmfb_write(fb, d->ctl + CTL_FLUSH, d->ctl_flush_sspp);
}

/* DisplayPort AUX channel */

static inline u32
msmfb_dp_read(struct msmfb *fb, u_int off)
{
	return (readl(fb->dp + off));
}

static inline void
msmfb_dp_write(struct msmfb *fb, u_int off, u32 val)
{
	writel(val, fb->dp + off);
}

/*
 * One AUX transaction, as msm's dp_aux.c does it: a native (DPCD) or I2C
 * read or write of up to 16 bytes, polled.  An I2C write keeps the bus for
 * the read that follows it.
 */
static int
msmfb_aux(struct msmfb *fb, bool i2c, bool read, u32 addr, u8 *buf,
    size_t len)
{
	u32 hdr[4], st, trans;
	size_t i, n;
	int us;

	if (len == 0 || len > 16)
		return (-EINVAL);
	hdr[0] = ((addr >> 16) & 0xf) | (read ? 0x10 : 0);
	hdr[1] = (addr >> 8) & 0xff;
	hdr[2] = addr & 0xff;
	hdr[3] = len - 1;
	n = read ? 0 : len;
	for (i = 0; i < n + 4; i++)
		msmfb_dp_write(fb, DP_AUX_DATA,
		    ((i < 4 ? hdr[i] : buf[i - 4]) << 8) |
		    (i == 0 ? DP_AUX_DATA_INDEX_WRITE : 0));

	msmfb_dp_write(fb, DP_AUX_TRANS_CTRL, 0);
	(void)msmfb_dp_read(fb, DP_PHY_AUX_INTR_STATUS);
	msmfb_dp_write(fb, DP_PHY_AUX_INTR_CLEAR, 0x1f);
	msmfb_dp_write(fb, DP_PHY_AUX_INTR_CLEAR, 0x9f);
	msmfb_dp_write(fb, DP_PHY_AUX_INTR_CLEAR, 0);
	/* Acknowledge stale events; the masks stay clear. */
	st = msmfb_dp_read(fb, DP_INTR_STATUS) &
	    (DP_INTR_AUX_DONE | DP_INTR_AUX_FAILED);
	msmfb_dp_write(fb, DP_INTR_STATUS, st << 1);

	trans = DP_AUX_TRANS_GO;
	if (i2c) {
		trans |= DP_AUX_TRANS_I2C | DP_AUX_TRANS_NO_SEND_ADDR;
		if (!read)
			trans |= DP_AUX_TRANS_NO_SEND_STOP;
	}
	msmfb_dp_write(fb, DP_AUX_TRANS_CTRL, trans);

	for (us = 0; us < DP_AUX_TIMEOUT_US; us += 20) {
		st = msmfb_dp_read(fb, DP_INTR_STATUS) &
		    (DP_INTR_AUX_DONE | DP_INTR_AUX_FAILED);
		if (st != 0)
			break;
		DELAY(20);
	}
	msmfb_dp_write(fb, DP_INTR_STATUS, st << 1);
	if (st == 0)
		return (-ETIMEDOUT);
	if ((st & DP_INTR_AUX_FAILED) != 0)
		return (-EIO);
	if (!read)
		return (0);

	/* The reply, from the FIFO's start; its first word is not data. */
	msmfb_dp_write(fb, DP_AUX_TRANS_CTRL,
	    msmfb_dp_read(fb, DP_AUX_TRANS_CTRL) & ~DP_AUX_TRANS_GO);
	msmfb_dp_write(fb, DP_AUX_DATA, DP_AUX_DATA_INDEX_WRITE |
	    DP_AUX_DATA_READ);
	(void)msmfb_dp_read(fb, DP_AUX_DATA);
	for (i = 0; i < len; i++)
		buf[i] = (msmfb_dp_read(fb, DP_AUX_DATA) >> 8) & 0xff;
	return (0);
}

/* The monitor's EDID, read through the sink's I2C bus, or NULL. */
static const struct drm_edid *
msmfb_read_edid(struct msmfb *fb)
{
	u8 *edid, off;
	size_t len, pos;
	int error;

	len = EDID_LENGTH * 2;		/* the base block and one extension */
	edid = kzalloc(len, GFP_KERNEL);
	if (edid == NULL)
		return (NULL);
	for (pos = 0; pos < len; pos += 16) {
		if (pos == EDID_LENGTH && edid[0x7e] == 0) {
			len = EDID_LENGTH;
			break;
		}
		off = pos;
		error = msmfb_aux(fb, true, false, DP_EDID_ADDR, &off, 1);
		if (error == 0)
			error = msmfb_aux(fb, true, true, DP_EDID_ADDR,
			    edid + pos, 16);
		if (error != 0) {
			dev_warn(fb->drm.dev, "EDID read at %zu failed: %d\n",
			    pos, error);
			kfree(edid);
			return (NULL);
		}
	}
	fb->edid = drm_edid_alloc(edid, len);
	kfree(edid);
	if (fb->edid != NULL && !drm_edid_valid(fb->edid)) {
		dev_warn(fb->drm.dev, "the monitor's EDID is not valid\n");
		drm_edid_free(fb->edid);
		fb->edid = NULL;
	}
	return (fb->edid);
}

/* Report the sink and the monitor. */
static void
msmfb_probe_sink(struct msmfb *fb)
{
	u8 dpcd[16];
	int error;

	error = msmfb_aux(fb, false, true, DP_DPCD_REV, dpcd, sizeof(dpcd));
	if (error != 0) {
		dev_warn(fb->drm.dev, "DPCD read failed: %d\n", error);
		return;
	}
	dev_info(fb->drm.dev, "DisplayPort %d.%d sink, up to %d lanes at "
	    "%d.%02d Gb/s\n", dpcd[DP_DPCD_REV] >> 4, dpcd[DP_DPCD_REV] & 0xf,
	    dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK,
	    dpcd[DP_MAX_LINK_RATE] * 27 / 100,
	    dpcd[DP_MAX_LINK_RATE] * 27 % 100);
	if (msmfb_read_edid(fb) != NULL)
		dev_info(fb->drm.dev, "monitor EDID read, %zu bytes\n",
		    drm_edid_raw(fb->edid)->extensions != 0 ? (size_t)256 :
		    (size_t)EDID_LENGTH);
}

/* GEM objects */

static void
msmfb_bo_free(struct drm_gem_object *obj)
{
	struct msmfb_bo *bo = to_msmfb_bo(obj);
	u_int i;

	if (bo->vaddr != NULL)
		vunmap(bo->vaddr);
	if (bo->page != NULL) {
		/* The device pager wanted them managed; give them back as got. */
		for (i = 0; i < bo->npages; i++)
#ifdef PAGE_IS_LKPI_PAGE
			bo->pages[i]->vm_page->oflags |= VPO_UNMANAGED;
#else
			bo->pages[i]->oflags |= VPO_UNMANAGED;
#endif
		__free_pages(bo->page, bo->order);
	}
	kvfree(bo->pages);
	drm_gem_object_release(obj);
	kfree(bo);
}

static struct sg_table *
msmfb_bo_get_sg_table(struct drm_gem_object *obj)
{
	struct msmfb_bo *bo = to_msmfb_bo(obj);

	return (drm_prime_pages_to_sg(obj->dev, bo->pages, bo->npages));
}

static int
msmfb_bo_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	iosys_map_set_vaddr(map, to_msmfb_bo(obj)->vaddr);
	return (0);
}

static vm_fault_t
msmfb_bo_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct msmfb_bo *bo = to_msmfb_bo(vma->vm_private_data);
	unsigned long addr;
	vm_fault_t r, ret;
	pgoff_t idx;

	idx = (vmf->address - vma->vm_start) >> PAGE_SHIFT;
	if (idx >= bo->npages)
		return (VM_FAULT_SIGBUS);

	/* Map the rest of the buffer as well; only the first page must succeed. */
	VM_OBJECT_WLOCK(vma->vm_obj);
	for (addr = vmf->address; idx < bo->npages; addr += PAGE_SIZE, idx++) {
		r = lkpi_vmf_insert_pfn_prot_locked(vma, addr,
		    page_to_pfn(bo->pages[idx]), vma->vm_page_prot);
		if (addr == vmf->address)
			ret = r;
		if ((r & VM_FAULT_ERROR) != 0)
			break;
	}
	VM_OBJECT_WUNLOCK(vma->vm_obj);
	return (ret);
}

static const struct vm_operations_struct msmfb_vm_ops = {
	.fault = msmfb_bo_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static int
msmfb_bo_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	vm_flags_set(vma, VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	/* As the kernel's mapping: the display reads memory, not caches. */
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	return (0);
}

static const struct drm_gem_object_funcs msmfb_bo_funcs = {
	.free = msmfb_bo_free,
	.get_sg_table = msmfb_bo_get_sg_table,
	.vmap = msmfb_bo_vmap,
	.mmap = msmfb_bo_mmap,
	.vm_ops = &msmfb_vm_ops,
};

static struct msmfb_bo *
msmfb_bo_create(struct drm_device *drm, size_t size)
{
	struct msmfb_bo *bo;
	u_int i;

	size = round_up(size, PAGE_SIZE);
	if (size == 0)
		return (ERR_PTR(-EINVAL));
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (bo == NULL)
		return (ERR_PTR(-ENOMEM));
	bo->base.funcs = &msmfb_bo_funcs;
	drm_gem_private_object_init(drm, &bo->base, size);

	bo->order = get_order(size);
	bo->npages = size >> PAGE_SHIFT;
	bo->pages = kvcalloc(bo->npages, sizeof(*bo->pages), GFP_KERNEL);
	if (bo->pages == NULL)
		goto fail;
	bo->page = alloc_pages(GFP_KERNEL | GFP_DMA32 | __GFP_ZERO, bo->order);
	if (bo->page == NULL)
		goto fail;
	for (i = 0; i < bo->npages; i++) {
		bo->pages[i] = nth_page(bo->page, i);
		/*
		 * The device pager that maps the pages into user space
		 * tracks their mappings, which it can only do for managed
		 * pages.
		 */
#ifdef PAGE_IS_LKPI_PAGE
		bo->pages[i]->vm_page->oflags &= ~VPO_UNMANAGED;
#else
		bo->pages[i]->oflags &= ~VPO_UNMANAGED;
#endif
	}
	/*
	 * Making them write-combining also writes the zeroing back from the
	 * CPU's caches.
	 */
	bo->vaddr = vmap(bo->pages, bo->npages, VM_MAP,
	    pgprot_writecombine(PAGE_KERNEL));
	if (bo->vaddr == NULL)
		goto fail;
	return (bo);

fail:
	drm_gem_object_put(&bo->base);
	return (ERR_PTR(-ENOMEM));
}

static int
msmfb_dumb_create(struct drm_file *file, struct drm_device *drm,
    struct drm_mode_create_dumb *args)
{
	struct msmfb_bo *bo;
	int error;

	args->pitch = roundup(args->width * DIV_ROUND_UP(args->bpp, 8), 64);
	args->size = (uint64_t)args->pitch * args->height;
	bo = msmfb_bo_create(drm, args->size);
	if (IS_ERR(bo))
		return (PTR_ERR(bo));
	error = drm_gem_handle_create(file, &bo->base, &args->handle);
	drm_gem_object_put(&bo->base);
	return (error);
}

/* Plane */

static int
msmfb_plane_atomic_check(struct drm_plane *plane,
    struct drm_atomic_state *state)
{
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state,
	    plane);
	struct drm_crtc_state *crtc_state = NULL;

	if (new->crtc != NULL)
		crtc_state = drm_atomic_get_new_crtc_state(state, new->crtc);
	/* The pipe scans out whole buffers: no scaling or positioning. */
	return (drm_atomic_helper_check_plane_state(new, crtc_state,
	    DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING, false, false));
}

static void
msmfb_plane_atomic_update(struct drm_plane *plane,
    struct drm_atomic_state *state)
{
	struct msmfb *fb = container_of(plane, struct msmfb, plane);
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state,
	    plane);
	struct drm_framebuffer *dfb = new->fb;
	struct msmfb_bo *bo;
	unsigned long flags;
	vm_paddr_t addr;
	int idx;

	if (!drm_dev_enter(&fb->drm, &idx))
		return;
	spin_lock_irqsave(&fb->lock, flags);
	if (dfb == NULL || !new->visible) {
		/* No client buffer: show the console. */
		msmfb_scanout(fb, fb->console_addr, fb->console_stride);
	} else {
		bo = to_msmfb_bo(drm_gem_fb_get_obj(dfb, 0));
		addr = page_to_phys(bo->page) + dfb->offsets[0] +
		    (new->src.y1 >> 16) * dfb->pitches[0] +
		    (new->src.x1 >> 16) * MSMFB_CPP;
		msmfb_scanout(fb, addr, dfb->pitches[0]);
	}
	spin_unlock_irqrestore(&fb->lock, flags);
	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs msmfb_plane_helper_funcs = {
	.atomic_check = msmfb_plane_atomic_check,
	.atomic_update = msmfb_plane_atomic_update,
	.atomic_disable = msmfb_plane_atomic_update,
};

static const struct drm_plane_funcs msmfb_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

/* CRTC */

/*
 * The interface's vsync: a vblank, and the end of a flip once the pipe's
 * flush bit has cleared, which it does as the pipe latches the new address.
 */
static irqreturn_t
msmfb_irq(int irq, void *arg)
{
	struct msmfb *fb = arg;
	const struct msm_fbsd_disp *d = fb->disp;
	u32 status;

	status = msmfb_read(fb, MDP_INTR_STATUS) & msmfb_read(fb, MDP_INTR_EN);
	if ((status & d->intr_vsync) == 0)
		return (IRQ_NONE);
	msmfb_write(fb, MDP_INTR_CLEAR, d->intr_vsync);

	drm_crtc_handle_vblank(&fb->crtc);
	spin_lock(&fb->drm.event_lock);
	if (fb->event != NULL &&
	    (msmfb_read(fb, d->ctl + CTL_FLUSH) & d->ctl_flush_sspp) == 0) {
		drm_crtc_send_vblank_event(&fb->crtc, fb->event);
		fb->event = NULL;
		drm_crtc_vblank_put(&fb->crtc);
	}
	spin_unlock(&fb->drm.event_lock);
	return (IRQ_HANDLED);
}

static void
msmfb_vsync_intr(struct msmfb *fb, bool on)
{
	const struct msm_fbsd_disp *d = fb->disp;
	unsigned long flags;
	u32 en;

	spin_lock_irqsave(&fb->lock, flags);
	en = msmfb_read(fb, MDP_INTR_EN);
	if (on) {
		/* Not for a vsync that happened while it was off. */
		msmfb_write(fb, MDP_INTR_CLEAR, d->intr_vsync);
		en |= d->intr_vsync;
	} else
		en &= ~d->intr_vsync;
	msmfb_write(fb, MDP_INTR_EN, en);
	spin_unlock_irqrestore(&fb->lock, flags);
}

static int
msmfb_enable_vblank(struct drm_crtc *crtc)
{
	msmfb_vsync_intr(container_of(crtc, struct msmfb, crtc), true);
	return (0);
}

static void
msmfb_disable_vblank(struct drm_crtc *crtc)
{
	msmfb_vsync_intr(container_of(crtc, struct msmfb, crtc), false);
}

static enum drm_mode_status
msmfb_crtc_mode_valid(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
	struct msmfb *fb = container_of(crtc, struct msmfb, crtc);

	if (mode->hdisplay != fb->mode.hdisplay ||
	    mode->vdisplay != fb->mode.vdisplay)
		return (MODE_ONE_SIZE);
	return (MODE_OK);
}

static int
msmfb_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);

	if (!new->enable)
		return (0);
	return (drm_atomic_helper_check_crtc_primary_plane(new));
}

/* The flip's event goes out at the vsync where the pipe latches it. */
static void
msmfb_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct msmfb *fb = container_of(crtc, struct msmfb, crtc);
	struct drm_pending_vblank_event *event = crtc->state->event;

	if (event == NULL)
		return;
	crtc->state->event = NULL;
	spin_lock_irq(&crtc->dev->event_lock);
	if (drm_crtc_vblank_get(crtc) != 0)
		drm_crtc_send_vblank_event(crtc, event);
	else {
		/* The one before cannot be pending: commits wait for flips. */
		WARN_ON_ONCE(fb->event != NULL);
		fb->event = event;
	}
	spin_unlock_irq(&crtc->dev->event_lock);
}

static void
msmfb_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	drm_crtc_vblank_on(crtc);
}

static void
msmfb_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct msmfb *fb = container_of(crtc, struct msmfb, crtc);

	drm_crtc_vblank_off(crtc);
	/* Complete a flip that vblank_off left without its vsync. */
	spin_lock_irq(&crtc->dev->event_lock);
	if (fb->event != NULL) {
		drm_crtc_send_vblank_event(crtc, fb->event);
		fb->event = NULL;
		drm_crtc_vblank_put(crtc);
	}
	if (crtc->state->event != NULL) {
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}
	spin_unlock_irq(&crtc->dev->event_lock);
}

static const struct drm_crtc_helper_funcs msmfb_crtc_helper_funcs = {
	.mode_valid = msmfb_crtc_mode_valid,
	.atomic_check = msmfb_crtc_atomic_check,
	.atomic_flush = msmfb_crtc_atomic_flush,
	.atomic_enable = msmfb_crtc_atomic_enable,
	.atomic_disable = msmfb_crtc_atomic_disable,
};

static const struct drm_crtc_funcs msmfb_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = msmfb_enable_vblank,
	.disable_vblank = msmfb_disable_vblank,
};

/* Encoder and connector */

static const struct drm_encoder_funcs msmfb_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int
msmfb_connector_get_modes(struct drm_connector *connector)
{
	struct msmfb *fb = container_of(connector, struct msmfb, connector);

	/* The monitor's identity; the mode stays the firmware's for now. */
	drm_edid_connector_update(connector, fb->edid);
	return (drm_connector_helper_get_modes_fixed(connector, &fb->mode));
}

static const struct drm_connector_helper_funcs msmfb_connector_helper_funcs = {
	.get_modes = msmfb_connector_get_modes,
};

static const struct drm_connector_funcs msmfb_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_framebuffer_funcs msmfb_fb_funcs = {
	.destroy = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
};

static struct drm_framebuffer *
msmfb_fb_create(struct drm_device *drm, struct drm_file *file,
    const struct drm_mode_fb_cmd2 *cmd)
{
	struct drm_framebuffer *dfb;
	struct drm_gem_object *obj;
	uint64_t need;
	int error;

	/* The pipe is set up for 32 bpp RGB, by the firmware. */
	if (cmd->pixel_format != DRM_FORMAT_XRGB8888 &&
	    cmd->pixel_format != DRM_FORMAT_ARGB8888)
		return (ERR_PTR(-EINVAL));
	if (cmd->modifier[0] != DRM_FORMAT_MOD_LINEAR &&
	    (cmd->flags & DRM_MODE_FB_MODIFIERS) != 0)
		return (ERR_PTR(-EINVAL));

	/*
	 * Only our buffers can be scanned out: others, such as the GPU's,
	 * are not contiguous.  The buffer must cover the framebuffer.
	 */
	obj = drm_gem_object_lookup(file, cmd->handles[0]);
	if (obj == NULL)
		return (ERR_PTR(-ENOENT));
	need = (uint64_t)cmd->offsets[0] +
	    (uint64_t)cmd->pitches[0] * (cmd->height - 1) +
	    (uint64_t)cmd->width * MSMFB_CPP;
	if (obj->funcs != &msmfb_bo_funcs || need > obj->size ||
	    cmd->pitches[0] > 0xffff) {
		drm_gem_object_put(obj);
		return (ERR_PTR(-EINVAL));
	}
	drm_gem_object_put(obj);

	dfb = kzalloc(sizeof(*dfb), GFP_KERNEL);
	if (dfb == NULL)
		return (ERR_PTR(-ENOMEM));
	error = drm_gem_fb_init_with_funcs(drm, dfb, file, cmd, &msmfb_fb_funcs);
	if (error != 0) {
		kfree(dfb);
		return (ERR_PTR(error));
	}
	return (dfb);
}

static const struct drm_mode_config_funcs msmfb_mode_config_funcs = {
	.fb_create = msmfb_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/*
 * vt(4) keeps drawing text in KD_GRAPHICS mode, into the firmware
 * framebuffer.  Keep it off while a client is DRM master, as sysfbdrm does;
 * vt redraws everything when it gets the display back.
 */
static void
msmfb_master_set(struct drm_device *drm, struct drm_file *file,
    bool from_open)
{
	struct msmfb *fb = to_msmfb(drm);
	struct fb_info *info;

	if (fb->vt_frozen || main_vd == NULL || main_vd->vd_driver == NULL ||
	    strcmp(main_vd->vd_driver->vd_name, "efifb") != 0)
		return;
	info = main_vd->vd_softc;
	if ((info->fb_flags & FB_FLAG_NOWRITE) != 0)
		return;
	vt_freeze_main_vd(info->fb_pbase, info->fb_size);
	fb->vt_frozen = (info->fb_flags & FB_FLAG_NOWRITE) != 0;
}

static void
msmfb_master_drop(struct drm_device *drm, struct drm_file *file)
{
	struct msmfb *fb = to_msmfb(drm);
	unsigned long flags;

	/*
	 * Show the console again: the client's buffers may go away, and
	 * nothing else will point the pipe back.
	 */
	spin_lock_irqsave(&fb->lock, flags);
	msmfb_scanout(fb, fb->console_addr, fb->console_stride);
	spin_unlock_irqrestore(&fb->lock, flags);
	if (fb->vt_frozen) {
		vt_unfreeze_main_vd();
		fb->vt_frozen = false;
	}
}

DEFINE_DRM_GEM_FOPS(msmfb_fops);

static const struct drm_driver msmfb_driver = {
	.driver_features = DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops = &msmfb_fops,
	.dumb_create = msmfb_dumb_create,
	.master_set = msmfb_master_set,
	.master_drop = msmfb_master_drop,
	.name = "msmfb",
	.desc = "Firmware-configured Qualcomm display",
	.date = "20260930",
	.major = 1,
	.minor = 0,
};

static int
msmfb_kms_init(struct msmfb *fb)
{
	struct drm_device *drm = &fb->drm;
	int error;

	/* The firmware doesn't report timings; assume 60 Hz and 96 dpi. */
	fb->mode = (struct drm_display_mode){ DRM_MODE_INIT(60, fb->width,
	    fb->height, DRM_MODE_RES_MM(fb->width, 96ul),
	    DRM_MODE_RES_MM(fb->height, 96ul)) };

	error = drmm_mode_config_init(drm);
	if (error != 0)
		return (error);
	drm->mode_config.min_width = drm->mode_config.max_width = fb->width;
	drm->mode_config.min_height = drm->mode_config.max_height = fb->height;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &msmfb_mode_config_funcs;

	error = drm_universal_plane_init(drm, &fb->plane, 0, &msmfb_plane_funcs,
	    msmfb_formats, nitems(msmfb_formats), msmfb_modifiers,
	    DRM_PLANE_TYPE_PRIMARY, NULL);
	if (error != 0)
		return (error);
	drm_plane_helper_add(&fb->plane, &msmfb_plane_helper_funcs);

	error = drm_crtc_init_with_planes(drm, &fb->crtc, &fb->plane, NULL,
	    &msmfb_crtc_funcs, NULL);
	if (error != 0)
		return (error);
	drm_crtc_helper_add(&fb->crtc, &msmfb_crtc_helper_funcs);

	error = drm_encoder_init(drm, &fb->encoder, &msmfb_encoder_funcs,
	    DRM_MODE_ENCODER_TMDS, NULL);
	if (error != 0)
		return (error);
	fb->encoder.possible_crtcs = drm_crtc_mask(&fb->crtc);

	error = drm_connector_init(drm, &fb->connector, &msmfb_connector_funcs,
	    DRM_MODE_CONNECTOR_DisplayPort);
	if (error != 0)
		return (error);
	drm_connector_helper_add(&fb->connector, &msmfb_connector_helper_funcs);
	error = drm_connector_attach_encoder(&fb->connector, &fb->encoder);
	if (error != 0)
		return (error);

	error = drm_vblank_init(drm, 1);
	if (error != 0)
		return (error);
	drm_mode_config_reset(drm);
	return (0);
}

/* Platform driver */

static int
msmfb_probe(struct platform_device *pdev)
{
	const struct msm_fbsd_disp *d = msm_fbsd_soc->disp;
	struct msmfb *fb;
	u32 size;
	int error, irq;

	if (d == NULL)
		return (-ENODEV);
	fb = devm_drm_dev_alloc(&pdev->dev, &msmfb_driver, struct msmfb, drm);
	if (IS_ERR(fb))
		return (PTR_ERR(fb));
	fb->disp = d;
	spin_lock_init(&fb->lock);
	fb->mdp = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(fb->mdp))
		return (PTR_ERR(fb->mdp));
	fb->dp = fb->mdp + d->dp;
	fb->mdp += d->mdp;

	/*
	 * Take over only the pipeline we know: an interface that is running
	 * and a source pipe that scans out something.
	 */
	fb->console_addr = msmfb_read(fb, d->sspp + SSPP_SRC0_ADDR);
	fb->console_stride = msmfb_read(fb, d->sspp + SSPP_SRC_YSTRIDE0) &
	    0xffff;
	size = msmfb_read(fb, d->sspp + SSPP_SRC_SIZE);
	fb->width = size & 0xffff;
	fb->height = size >> 16;
	if ((msmfb_read(fb, d->intf) & 1) == 0 || fb->console_addr == 0 ||
	    fb->width == 0 || fb->height == 0) {
		dev_info(&pdev->dev, "the firmware left no display running\n");
		return (-ENODEV);
	}

	msmfb_probe_sink(fb);
	error = msmfb_kms_init(fb);
	if (error != 0) {
		drm_edid_free(fb->edid);
		return (error);
	}
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return (irq);
	error = devm_request_irq(&pdev->dev, irq, msmfb_irq, 0, "msmfb", fb);
	if (error != 0)
		return (error);
	platform_set_drvdata(pdev, fb);
	error = drm_dev_register(&fb->drm, 0);
	if (error != 0)
		return (error);
	dev_info(&pdev->dev, "%ux%u display, scanning out %#x\n", fb->width,
	    fb->height, fb->console_addr);
	return (0);
}

static void
msmfb_remove(struct platform_device *pdev)
{
	struct msmfb *fb = platform_get_drvdata(pdev);

	drm_dev_unplug(&fb->drm);
	/* Disables the plane, which shows the console again. */
	drm_atomic_helper_shutdown(&fb->drm);
	msmfb_vsync_intr(fb, false);
	drm_edid_free(fb->edid);
	fb->edid = NULL;
}

static struct platform_driver msmfb_platform_driver = {
	.probe = msmfb_probe,
	.remove = msmfb_remove,
	.driver = {
		.name = "msmfb",
	},
};

bool
msm_fbsd_fb_busy(struct platform_device *pdev)
{
	struct msmfb *fb = platform_get_drvdata(pdev);

	return (fb != NULL && atomic_read(&fb->drm.open_count) > 0);
}

int
msm_fbsd_fb_register(void)
{
	return (platform_driver_register(&msmfb_platform_driver));
}

void
msm_fbsd_fb_unregister(void)
{
	platform_driver_unregister(&msmfb_platform_driver);
}
