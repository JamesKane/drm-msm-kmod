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
 * pipe, layer mixer, control path and interface to a DisplayPort output.  A
 * flip points the source pipe at the client's buffer and flushes it, which
 * takes effect at the next vsync; the interface's vsync interrupt drives
 * vblank.  With no client, the pipe shows the firmware framebuffer and the
 * console.
 *
 * The PHY and the link's rate and lanes stay as the firmware set them up; a
 * mode is the stream on that link.  Setting one goes as msm disables and
 * enables its output: the link idles at the end of a frame, the interface
 * stops and the link goes off; the pixel clock, the controller's stream
 * timing, MSA and transfer unit, the pipe and mixer sizes and the interface's
 * timing are set, all as msm works them out; and the link comes back on, is
 * trained, and carries the stream the interface starts again.  The firmware's
 * registers are kept, to put its mode back for the console when the client
 * goes.  With the CRTC inactive (DPMS off) the stream stops the same way,
 * and the monitor goes to sleep.
 *
 * The DisplayPort controller's AUX channel reads the sink's capabilities and
 * the monitor's EDID, which the connector reports.  Its transfers are polled,
 * with the controller's AUX interrupts masked, as the firmware left them;
 * only its hotplug events interrupt.  A DP-to-HDMI bridge keeps hotplug high
 * while it is there and signals its monitor coming and going with IRQ_HPD
 * pulses and its DPCD sink count, so the monitor is connected when both say
 * so.  When it comes back the link is trained again, at the rate and lane
 * count the firmware chose, as Linux's msm does on every plug: a bridge may
 * keep showing the picture without, but then no longer reports the link
 * locked.
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
#include <vm/vm_pageout.h>

#include <machine/bus.h>

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
#include <drm/display/drm_dp_helper.h>
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
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <drm/drm_vblank.h>

#include "msm_freebsd.h"
#include "msm_freebsd_dp_calc.h"

#define	MSMFB_CPP		4
#define	MSMFB_MAX_WIDTH		2560		/* of a layer mixer */
#define	MSMFB_MAX_PIXEL_KHZ	675000		/* msm's DP_MAX_PIXEL_CLK_KHZ */

/* DPU registers, relative to the MDP block. */
/* MDSS registers, relative to the display window. */
#define	MDSS_HW_INTR_STATUS	0x010
#define	 MDSS_INTR_MDP		(1u << 0)

#define	MDP_INTR_EN		0x010
#define	MDP_INTR_STATUS		0x014
#define	MDP_INTR_CLEAR		0x018
#define	SSPP_SRC_SIZE		0x000
#define	SSPP_OUT_SIZE		0x00c
#define	SSPP_SRC0_ADDR		0x014
#define	SSPP_SRC_YSTRIDE0	0x024
#define	LM_OUT_SIZE		0x004
#define	CTL_FLUSH		0x018
#define	 CTL_FLUSH_CTL		(1u << 17)
#define	 CTL_FLUSH_INTF		(1u << 31)
#define	CTL_INTF_FLUSH		0x110
#define	INTF_TIMING_ENGINE_EN	0x000
#define	INTF_CONFIG		0x004
#define	 INTF_CFG_ACTIVE_H_EN	(1u << 29)
#define	 INTF_CFG_ACTIVE_V_EN	(1u << 30)
#define	 INTF_CFG_PROG_FETCH	(1u << 31)
#define	INTF_HSYNC_CTL		0x008
#define	INTF_VSYNC_PERIOD_F0	0x00c
#define	INTF_VSYNC_PULSE_WIDTH_F0 0x014
#define	INTF_DISPLAY_V_START_F0	0x01c
#define	INTF_DISPLAY_V_END_F0	0x024
#define	INTF_ACTIVE_V_START_F0	0x02c
#define	INTF_ACTIVE_V_END_F0	0x034
#define	INTF_DISPLAY_HCTL	0x03c
#define	INTF_ACTIVE_HCTL	0x040
#define	INTF_POLARITY_CTL	0x050
#define	INTF_CONFIG2		0x060
#define	 INTF_CFG2_DATABUS_WIDEN (1u << 0)
#define	INTF_DISPLAY_DATA_HCTL	0x064
#define	INTF_ACTIVE_DATA_HCTL	0x068

/* A clock root's registers. */
#define	RCG_CMD			0x000
#define	 RCG_CMD_UPDATE		(1u << 0)
#define	RCG_CFG			0x004
#define	 RCG_CFG_MODE_MASK	(3u << 12)
#define	 RCG_CFG_MODE_DUAL_EDGE	(2u << 12)
#define	RCG_M			0x008
#define	RCG_N			0x00c
#define	RCG_D			0x010

/* DisplayPort controller registers, relative to the controller. */
#define	DP_INTR_STATUS		0x020		/* AHB block */
#define	DP_INTR_STATUS2		0x024
#define	 DP_INTR_IDLE_PATTERN_SENT (1u << 3)
#define	DP_AUX_CTRL		0x230		/* AUX block */
#define	 DP_AUX_CTRL_RESET	(1u << 1)
#define	DP_AUX_DATA		0x234
#define	 DP_AUX_DATA_READ	(1u << 0)
#define	 DP_AUX_DATA_INDEX_WRITE (1u << 31)
#define	DP_AUX_TRANS_CTRL	0x238
#define	 DP_AUX_TRANS_I2C	(1u << 8)
#define	 DP_AUX_TRANS_GO	(1u << 9)
#define	 DP_AUX_TRANS_NO_SEND_ADDR (1u << 10)
#define	 DP_AUX_TRANS_NO_SEND_STOP (1u << 11)
#define	DP_HPD_INT_STATUS	0x204
#define	 DP_HPD_STATE_MASK	0xe0000000u
#define	 DP_HPD_STATE_CONNECTED	0x40000000u
#define	 DP_HPD_EVENTS		0x0000000fu	/* plug, IRQ_HPD, replug, unplug */
#define	DP_HPD_INT_ACK		0x208
#define	DP_HPD_INT_MASK		0x20c
#define	DP_PHY_AUX_INTR_CLEAR	0x24c
#define	DP_MAINLINK_CTRL	0x400		/* link block */
#define	 DP_MAINLINK_ENABLE	(1u << 0)
#define	 DP_MAINLINK_RESET	(1u << 1)
#define	 DP_MAINLINK_FB_BOUNDARY_SEL (1u << 25)
#define	DP_STATE_CTRL		0x404
#define	 DP_STATE_SEND_VIDEO	(1u << 7)
#define	 DP_STATE_PUSH_IDLE	(1u << 8)
#define	DP_SOFTWARE_MVID	0x410
#define	DP_SOFTWARE_NVID	0x418
#define	DP_TOTAL_HOR_VER	0x41c
#define	DP_START_HOR_VER_FROM_SYNC 0x420
#define	DP_HSYNC_VSYNC_WIDTH_POLARITY 0x424
#define	 DP_HSYNC_ACTIVE_LOW	(1u << 15)
#define	 DP_VSYNC_ACTIVE_LOW	(1u << 31)
#define	DP_ACTIVE_HOR_VER	0x428
#define	DP_VALID_BOUNDARY	0x430
#define	DP_VALID_BOUNDARY_2	0x434
#define	DP_MAINLINK_READY	0x440
#define	 DP_READY_TRAINING_SHIFT 3
#define	DP_TU			0x44c

/* The PHY's transmit blocks, each two lanes. */
#define	PHY_TX_EMP_POST1_LVL	0x004
#define	PHY_TX_DRV_LVL		0x014
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

/*
 * What holds a mode: the DisplayPort pixel clock's M/N/D counter, the
 * controller's stream timing, MSA and transfer unit, the sizes of the pipe
 * and the mixer, and the interface's timing.
 */
struct msmfb_timing {
	u32	rcg_m, rcg_n, rcg_d, rcg_cfg;
	u32	dp_total, dp_start, dp_sync, dp_active;
	u32	dp_mvid, dp_nvid, dp_vb, dp_vb2, dp_tu;
	u32	size;
	u32	intf_hsync, intf_vperiod, intf_vpulse;
	u32	intf_disp_vstart, intf_disp_vend, intf_act_vstart, intf_act_vend;
	u32	intf_disp_hctl, intf_act_hctl, intf_polarity;
	u32	intf_config, intf_config2, intf_data_hctl, intf_act_data_hctl;
};

struct msmfb {
	struct drm_device		drm;
	const struct msm_fbsd_disp	*disp;
	void __iomem			*mdss;
	void __iomem			*mdp;
	void __iomem			*dp;
	struct mutex			sink_lock;	/* AUX, and below */
	const struct drm_edid		*edid;		/* or NULL */
	bool				connected;
	struct work_struct		hotplug_work;
	u_int				link_rate;	/* kHz, or 0 */
	u_int				lanes;
	u32				console_addr;	/* the firmware's scanout */
	u32				console_stride;
	struct mutex			mode_lock;	/* the timing, below */
	struct msmfb_timing		fw_timing;
	struct drm_display_mode		fw_mode;
	struct drm_display_mode		hw_mode;	/* what the timing is */
	bool				output_off;	/* by DPMS */
	struct drm_plane		plane;
	struct drm_crtc			crtc;
	struct drm_encoder		encoder;
	struct drm_connector		connector;
	spinlock_t			lock;		/* registers, below */
	struct drm_pending_vblank_event	*event;		/* of a latching flip */
	bool				vt_frozen;	/* by master_set */
};

#define	to_msmfb(d)	container_of(d, struct msmfb, drm)

/* A dumb buffer: physically contiguous pages below 4 GB. */
struct msmfb_bo {
	struct drm_gem_object	base;
	struct page		*page;
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
msmfb_aux_once(struct msmfb *fb, bool i2c, bool read, u32 addr, u8 *buf,
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

/*
 * A transfer, retried as the DRM helpers do, after resetting the AUX block
 * as msm does when one fails: a sink losing its link may miss one.
 */
static int
msmfb_aux(struct msmfb *fb, bool i2c, bool read, u32 addr, u8 *buf,
    size_t len)
{
	u32 ctrl;
	int error, tries;

	for (tries = 0; tries < (i2c ? 3 : 32); tries++) {
		error = msmfb_aux_once(fb, i2c, read, addr, buf, len);
		if (error == 0 || error == -EINVAL)
			break;
		ctrl = msmfb_dp_read(fb, DP_AUX_CTRL);
		msmfb_dp_write(fb, DP_AUX_CTRL, ctrl | DP_AUX_CTRL_RESET);
		usleep_range(1000, 1100);
		msmfb_dp_write(fb, DP_AUX_CTRL, ctrl & ~DP_AUX_CTRL_RESET);
	}
	return (error);
}

/* A native AUX write of one byte, for the DPCD. */
static int
msmfb_dpcd_writeb(struct msmfb *fb, u32 addr, u8 val)
{
	return (msmfb_aux(fb, false, false, addr, &val, 1));
}

/*
 * The PHY's drive level and pre-emphasis for each voltage swing and
 * pre-emphasis level, from Linux's phy-qcom-edp.c (DP, not eDP); 0xff where
 * swing and pre-emphasis together exceed level 3.
 */
static const u8 msmfb_swing_hbr_rbr[4][4] = {
	{ 0x07, 0x0f, 0x16, 0x1f },
	{ 0x11, 0x1e, 0x1f, 0xff },
	{ 0x16, 0x1f, 0xff, 0xff },
	{ 0x1f, 0xff, 0xff, 0xff }
};
static const u8 msmfb_emph_hbr_rbr[4][4] = {
	{ 0x00, 0x0e, 0x15, 0x1a },
	{ 0x00, 0x0e, 0x15, 0xff },
	{ 0x00, 0x0e, 0xff, 0xff },
	{ 0x04, 0xff, 0xff, 0xff }
};
static const u8 msmfb_swing_hbr2_hbr3[4][4] = {
	{ 0x02, 0x12, 0x16, 0x1a },
	{ 0x09, 0x19, 0x1f, 0xff },
	{ 0x10, 0x1f, 0xff, 0xff },
	{ 0x1f, 0xff, 0xff, 0xff }
};
static const u8 msmfb_emph_hbr2_hbr3[4][4] = {
	{ 0x00, 0x0c, 0x15, 0x1b },
	{ 0x02, 0x0e, 0x16, 0xff },
	{ 0x02, 0x11, 0xff, 0xff },
	{ 0x04, 0xff, 0xff, 0xff }
};

struct msmfb_train {
	u8	bw;		/* DPCD LINK_BW_SET */
	u8	lanes;
	u8	v, p;		/* voltage swing and pre-emphasis levels */
	u8	dpcd[DP_RECEIVER_CAP_SIZE];
};

/* Drive the lanes at levels v and p, and tell the sink. */
static int
msmfb_train_levels(struct msmfb *fb, struct msmfb_train *t)
{
	const struct msm_fbsd_disp *d = fb->disp;
	u8 set[4], swing, emph;
	int i;

	if (t->bw <= DP_LINK_BW_2_7) {
		swing = msmfb_swing_hbr_rbr[t->v][t->p];
		emph = msmfb_emph_hbr_rbr[t->v][t->p];
	} else {
		swing = msmfb_swing_hbr2_hbr3[t->v][t->p];
		emph = msmfb_emph_hbr2_hbr3[t->v][t->p];
	}
	if (swing == 0xff || emph == 0xff)
		return (-EINVAL);
	for (i = 0; i < 2; i++) {
		writel(swing, fb->mdss + d->dp_phy_tx[i] + PHY_TX_DRV_LVL);
		writel(emph, fb->mdss + d->dp_phy_tx[i] + PHY_TX_EMP_POST1_LVL);
	}
	for (i = 0; i < t->lanes; i++)
		set[i] = t->v | (t->p << DP_TRAIN_PRE_EMPHASIS_SHIFT) |
		    (t->v == 3 ? DP_TRAIN_MAX_SWING_REACHED : 0) |
		    (t->p == 3 ? DP_TRAIN_MAX_PRE_EMPHASIS_REACHED : 0);
	return (msmfb_aux(fb, false, false, DP_TRAINING_LANE0_SET, set,
	    t->lanes));
}

/* The levels the sink asks for, the highest of any lane's, within level 3. */
static void
msmfb_train_adjust(struct msmfb_train *t, const u8 *status)
{
	int i;

	t->v = t->p = 0;
	for (i = 0; i < t->lanes; i++) {
		t->v = max(t->v, drm_dp_get_adjust_request_voltage(status, i) >>
		    DP_TRAIN_VOLTAGE_SWING_SHIFT);
		t->p = max(t->p,
		    drm_dp_get_adjust_request_pre_emphasis(status, i) >>
		    DP_TRAIN_PRE_EMPHASIS_SHIFT);
	}
	if (t->v + t->p > 3)
		t->p = 3 - t->v;
}

/* Have the controller send training pattern n (0 for none). */
static int
msmfb_train_pattern(struct msmfb *fb, int n)
{
	int us;

	msmfb_dp_write(fb, DP_STATE_CTRL, 0);
	if (n == 0)
		return (0);
	msmfb_dp_write(fb, DP_STATE_CTRL, 1u << (n - 1));
	for (us = 0; us < 10000; us += 10) {
		if ((msmfb_dp_read(fb, DP_MAINLINK_READY) &
		    ((1u << (n - 1)) << DP_READY_TRAINING_SHIFT)) != 0)
			return (0);
		DELAY(10);
	}
	return (-ETIMEDOUT);
}

/*
 * The waits before reading the link status, as drm_dp_link_train_*_delay()
 * work them out (which need a drm_dp_aux); intervals over 4 count as 4.
 */
static void
msmfb_train_delay(const struct msmfb_train *t, bool eq)
{
	u_int rd, us;

	rd = min(t->dpcd[DP_TRAINING_AUX_RD_INTERVAL] &
	    DP_TRAINING_AUX_RD_MASK, 4u);
	if (!eq && t->dpcd[DP_DPCD_REV] >= DP_DPCD_REV_14)
		us = 100;
	else if (rd == 0)
		us = eq ? 400 : 100;
	else
		us = rd * 4000;
	usleep_range(us, us * 2);
}

static int
msmfb_train_status(struct msmfb *fb, u8 *status)
{
	/* DP_LINK_STATUS_SIZE bytes from DP_LANE0_1_STATUS, in AUX-sized reads. */
	return (msmfb_aux(fb, false, true, DP_LANE0_1_STATUS, status,
	    DP_LINK_STATUS_SIZE));
}

/*
 * Train the link, as msm's dp_ctrl.c does: clock recovery with pattern 1,
 * then channel equalization with the best pattern the sink has.
 */
static int
msmfb_link_train(struct msmfb *fb)
{
	struct msmfb_train t;
	u8 status[DP_LINK_STATUS_SIZE], cfg[2], pattern;
	int error, tries, old_v, n;

	memset(&t, 0, sizeof(t));
	memset(status, 0, sizeof(status));
	tries = -1;
	error = msmfb_aux(fb, false, true, DP_DPCD_REV, t.dpcd, 15);
	if (error != 0)
		return (error);
	/* The rate and lanes the firmware trained at. */
	error = msmfb_aux(fb, false, true, DP_LINK_BW_SET, cfg, 2);
	if (error != 0)
		return (error);
	t.bw = cfg[0];
	t.lanes = fb->lanes != 0 ? fb->lanes : cfg[1] & DP_LANE_COUNT_MASK;
	if (t.lanes == 0 || t.lanes > 4 || t.bw == 0)
		return (-EINVAL);

	/* Link configuration, as msm_dp_aux_link_configure() and after. */
	cfg[1] = t.lanes | DP_LANE_COUNT_ENHANCED_FRAME_EN;
	error = msmfb_aux(fb, false, false, DP_LINK_BW_SET, cfg, 2);
	if (error != 0)
		return (error);
	cfg[0] = drm_dp_max_downspread(t.dpcd) ? DP_SPREAD_AMP_0_5 : 0;
	cfg[1] = DP_SET_ANSI_8B10B;
	error = msmfb_aux(fb, false, false, DP_DOWNSPREAD_CTRL, cfg, 2);
	if (error != 0)
		return (error);

	/* Clock recovery. */
	error = msmfb_train_pattern(fb, 1);
	if (error != 0)
		dev_warn(fb->drm.dev, "link training: no pattern 1, ready "
		    "%#x\n", msmfb_dp_read(fb, DP_MAINLINK_READY));
	if (error == 0)
		error = msmfb_dpcd_writeb(fb, DP_TRAINING_PATTERN_SET,
		    DP_TRAINING_PATTERN_1 | DP_LINK_SCRAMBLING_DISABLE);
	if (error == 0)
		error = msmfb_train_levels(fb, &t);
	for (tries = 0, old_v = t.v; error == 0; tries++) {
		msmfb_train_delay(&t, false);
		if ((error = msmfb_train_status(fb, status)) != 0)
			break;
		if (drm_dp_clock_recovery_ok(status, t.lanes))
			break;
		if (t.v >= 3 || tries >= 4) {
			error = -ETIMEDOUT;
			break;
		}
		msmfb_train_adjust(&t, status);
		if (t.v != old_v) {
			tries = 0;
			old_v = t.v;
		}
		error = msmfb_train_levels(fb, &t);
	}
	if (error != 0) {
		dev_warn(fb->drm.dev, "link training: clock recovery failed: "
		    "%d after %d tries, swing %d pre-emphasis %d, lanes %#04x "
		    "%#04x, ready %#x\n", error, tries, t.v, t.p, status[0],
		    status[1], msmfb_dp_read(fb, DP_MAINLINK_READY));
		goto out;
	}

	/* Channel equalization. */
	if (drm_dp_tps4_supported(t.dpcd)) {
		pattern = DP_TRAINING_PATTERN_4;
		n = 4;
	} else if (drm_dp_tps3_supported(t.dpcd)) {
		pattern = DP_TRAINING_PATTERN_3;
		n = 3;
	} else {
		pattern = DP_TRAINING_PATTERN_2;
		n = 2;
	}
	error = msmfb_train_pattern(fb, n);
	if (error == 0)
		error = msmfb_dpcd_writeb(fb, DP_TRAINING_PATTERN_SET,
		    pattern | (pattern != DP_TRAINING_PATTERN_4 ?
		    DP_LINK_SCRAMBLING_DISABLE : 0));
	for (tries = 0; error == 0; tries++) {
		msmfb_train_delay(&t, true);
		if ((error = msmfb_train_status(fb, status)) != 0)
			break;
		if (drm_dp_channel_eq_ok(status, t.lanes))
			break;
		if (tries >= 5) {
			error = -ETIMEDOUT;
			break;
		}
		msmfb_train_adjust(&t, status);
		error = msmfb_train_levels(fb, &t);
	}
	if (error != 0)
		dev_warn(fb->drm.dev, "link training: equalization failed: "
		    "%d\n", error);

out:
	/* Back to the video, trained or not. */
	(void)msmfb_train_pattern(fb, 0);
	(void)msmfb_dpcd_writeb(fb, DP_TRAINING_PATTERN_SET,
	    DP_TRAINING_PATTERN_DISABLE);
	msmfb_train_delay(&t, true);
	msmfb_dp_write(fb, DP_STATE_CTRL, DP_STATE_SEND_VIDEO);
	if (error == 0)
		dev_info(fb->drm.dev, "link trained: %d lanes at %d.%02d Gb/s, "
		    "swing %d, pre-emphasis %d\n", t.lanes, t.bw * 27 / 100,
		    t.bw * 27 % 100, t.v, t.p);
	return (error);
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

/* Report the sink's capabilities. */
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

	/* The link the firmware trained, which modes are set on. */
	if (msmfb_aux(fb, false, true, DP_LINK_BW_SET, dpcd, 2) != 0)
		return;
	fb->link_rate = drm_dp_bw_code_to_link_rate(dpcd[0]);
	fb->lanes = dpcd[1] & DP_LANE_COUNT_MASK;
	if ((fb->link_rate != 162000 && fb->link_rate != 270000 &&
	    fb->link_rate != 540000 && fb->link_rate != 810000) ||
	    (fb->lanes != 1 && fb->lanes != 2 && fb->lanes != 4))
		fb->link_rate = 0;
	else
		dev_info(fb->drm.dev, "link: %u lanes at %u.%02u Gb/s\n",
		    fb->lanes, fb->link_rate / 100000,
		    fb->link_rate / 1000 % 100);
}

/*
 * Find out whether a monitor is connected, and read its EDID if one is; with
 * sink_lock held.  Returns whether anything changed.
 */
static bool
msmfb_update_sink(struct msmfb *fb)
{
	u8 st[6], irq;
	bool connected, changed;
	u32 hpd;

	hpd = msmfb_dp_read(fb, DP_HPD_INT_STATUS) & DP_HPD_STATE_MASK;
	connected = false;
	if (hpd == DP_HPD_STATE_CONNECTED &&
	    msmfb_aux(fb, false, true, DP_SINK_COUNT, st, sizeof(st)) == 0) {
		connected = DP_GET_SINK_COUNT(st[0]) > 0;
		dev_info(fb->drm.dev, "sink count %d, service IRQ %#x, lanes "
		    "%#04x %#04x, align %#x\n", DP_GET_SINK_COUNT(st[0]), st[1],
		    st[2], st[3], st[4]);
		/* Clear the service requests it reported. */
		irq = st[1];
		if (irq != 0)
			(void)msmfb_aux(fb, false, false,
			    DP_DEVICE_SERVICE_IRQ_VECTOR, &irq, 1);
	}
	changed = connected != fb->connected;
	if (connected) {
		drm_edid_free(fb->edid);
		fb->edid = NULL;
		if (msmfb_read_edid(fb) != NULL)
			dev_info(fb->drm.dev, "monitor EDID read, %zu bytes\n",
			    drm_edid_raw(fb->edid)->extensions != 0 ?
			    (size_t)256 : (size_t)EDID_LENGTH);
		changed = true;		/* it may be another monitor */
	} else if (fb->edid != NULL) {
		drm_edid_free(fb->edid);
		fb->edid = NULL;
	}
	fb->connected = connected;
	return (changed);
}

/*
 * Whether the sink holds the link: the lanes locked and aligned.  A bridge
 * whose monitor comes back relocks within moments, so look a little later.
 */
static void
msmfb_check_link(struct msmfb *fb)
{
	u8 st[3];

	msleep(300);
	if (msmfb_aux(fb, false, true, DP_LANE0_1_STATUS, st, sizeof(st)) != 0)
		return;
	dev_info(fb->drm.dev, "link %s: lanes %#04x %#04x, align %#x\n",
	    st[0] == 0x77 && st[1] == 0x77 &&
	    (st[2] & DP_INTERLANE_ALIGN_DONE) != 0 ? "locked" : "NOT locked",
	    st[0], st[1], st[2]);
}

static void
msmfb_hotplug_work(struct work_struct *work)
{
	struct msmfb *fb = container_of(work, struct msmfb, hotplug_work);
	bool changed;

	mutex_lock(&fb->sink_lock);
	changed = msmfb_update_sink(fb);
	/* An output that is off is trained when it goes on. */
	if (fb->connected && !READ_ONCE(fb->output_off)) {
		(void)msmfb_link_train(fb);
		msmfb_check_link(fb);
	}
	mutex_unlock(&fb->sink_lock);
	dev_info(fb->drm.dev, "hotplug: monitor %s\n",
	    fb->connected ? "connected" : "disconnected");
	if (changed)
		drm_kms_helper_hotplug_event(&fb->drm);
}

/* Mode setting */

enum msmfb_block { MSMFB_RCG, MSMFB_DP, MSMFB_SSPP, MSMFB_LM, MSMFB_INTF };

#define	MSMFB_TREG(b, r, f)	{ b, r, offsetof(struct msmfb_timing, f) }

/* Where the timing goes, in the order it is written. */
static const struct {
	enum msmfb_block	block;
	u_int			reg;
	size_t			field;
} msmfb_timing_regs[] = {
	MSMFB_TREG(MSMFB_RCG, RCG_M, rcg_m),
	MSMFB_TREG(MSMFB_RCG, RCG_N, rcg_n),
	MSMFB_TREG(MSMFB_RCG, RCG_D, rcg_d),
	MSMFB_TREG(MSMFB_RCG, RCG_CFG, rcg_cfg),
	MSMFB_TREG(MSMFB_DP, DP_TOTAL_HOR_VER, dp_total),
	MSMFB_TREG(MSMFB_DP, DP_START_HOR_VER_FROM_SYNC, dp_start),
	MSMFB_TREG(MSMFB_DP, DP_HSYNC_VSYNC_WIDTH_POLARITY, dp_sync),
	MSMFB_TREG(MSMFB_DP, DP_ACTIVE_HOR_VER, dp_active),
	MSMFB_TREG(MSMFB_DP, DP_SOFTWARE_MVID, dp_mvid),
	MSMFB_TREG(MSMFB_DP, DP_SOFTWARE_NVID, dp_nvid),
	MSMFB_TREG(MSMFB_DP, DP_VALID_BOUNDARY, dp_vb),
	MSMFB_TREG(MSMFB_DP, DP_TU, dp_tu),
	MSMFB_TREG(MSMFB_DP, DP_VALID_BOUNDARY_2, dp_vb2),
	MSMFB_TREG(MSMFB_SSPP, SSPP_SRC_SIZE, size),
	MSMFB_TREG(MSMFB_SSPP, SSPP_OUT_SIZE, size),
	MSMFB_TREG(MSMFB_LM, LM_OUT_SIZE, size),
	MSMFB_TREG(MSMFB_INTF, INTF_HSYNC_CTL, intf_hsync),
	MSMFB_TREG(MSMFB_INTF, INTF_VSYNC_PERIOD_F0, intf_vperiod),
	MSMFB_TREG(MSMFB_INTF, INTF_VSYNC_PULSE_WIDTH_F0, intf_vpulse),
	MSMFB_TREG(MSMFB_INTF, INTF_DISPLAY_HCTL, intf_disp_hctl),
	MSMFB_TREG(MSMFB_INTF, INTF_DISPLAY_V_START_F0, intf_disp_vstart),
	MSMFB_TREG(MSMFB_INTF, INTF_DISPLAY_V_END_F0, intf_disp_vend),
	MSMFB_TREG(MSMFB_INTF, INTF_ACTIVE_HCTL, intf_act_hctl),
	MSMFB_TREG(MSMFB_INTF, INTF_ACTIVE_V_START_F0, intf_act_vstart),
	MSMFB_TREG(MSMFB_INTF, INTF_ACTIVE_V_END_F0, intf_act_vend),
	MSMFB_TREG(MSMFB_INTF, INTF_POLARITY_CTL, intf_polarity),
	MSMFB_TREG(MSMFB_INTF, INTF_CONFIG, intf_config),
	MSMFB_TREG(MSMFB_INTF, INTF_CONFIG2, intf_config2),
	MSMFB_TREG(MSMFB_INTF, INTF_DISPLAY_DATA_HCTL, intf_data_hctl),
	MSMFB_TREG(MSMFB_INTF, INTF_ACTIVE_DATA_HCTL, intf_act_data_hctl),
};

static void __iomem *
msmfb_block(struct msmfb *fb, enum msmfb_block block)
{
	const struct msm_fbsd_disp *d = fb->disp;

	switch (block) {
	case MSMFB_RCG:
		return (fb->mdss + d->dp_pixel_rcg);
	case MSMFB_DP:
		return (fb->dp);
	case MSMFB_SSPP:
		return (fb->mdp + d->sspp);
	case MSMFB_LM:
		return (fb->mdp + d->lm);
	case MSMFB_INTF:
	default:
		return (fb->mdp + d->intf);
	}
}

static inline u32 *
msmfb_treg_field(struct msmfb_timing *t, u_int i)
{
	return ((u32 *)((char *)t + msmfb_timing_regs[i].field));
}

static void
msmfb_read_timing(struct msmfb *fb, struct msmfb_timing *t)
{
	u_int i;

	for (i = 0; i < nitems(msmfb_timing_regs); i++)
		*msmfb_treg_field(t, i) = readl(msmfb_block(fb,
		    msmfb_timing_regs[i].block) + msmfb_timing_regs[i].reg);
}

/* How many pixels the interface's bus carries at once. */
static u_int
msmfb_bus_pixels(struct msmfb *fb)
{
	return ((fb->fw_timing.intf_config2 & INTF_CFG2_DATABUS_WIDEN) != 0 ?
	    2 : 1);
}

/* The pixel clock's parent, in kHz: the PHY's link clock, divided. */
static u_long
msmfb_pixel_parent(struct msmfb *fb)
{
	switch (fb->link_rate) {
	case 810000:
		return (fb->link_rate * 10 / 6);
	case 540000:
		return (fb->link_rate * 10 / 4);
	default:
		return (fb->link_rate * 10 / 2);
	}
}

static bool
msmfb_mode_same(const struct drm_display_mode *a,
    const struct drm_display_mode *b)
{
	return (drm_mode_match(a, b, DRM_MODE_MATCH_TIMINGS |
	    DRM_MODE_MATCH_CLOCK | DRM_MODE_MATCH_FLAGS));
}

/* The mode a timing is, for the firmware's. */
static void
msmfb_timing_mode(struct msmfb *fb, const struct msmfb_timing *t,
    struct drm_display_mode *m)
{
	u32 n;

	memset(m, 0, sizeof(*m));
	m->hdisplay = t->dp_active & 0xffff;
	m->vdisplay = t->dp_active >> 16;
	m->htotal = t->dp_total & 0xffff;
	m->vtotal = t->dp_total >> 16;
	m->hsync_start = m->htotal - (t->dp_start & 0xffff);
	m->vsync_start = m->vtotal - (t->dp_start >> 16);
	m->hsync_end = m->hsync_start + (t->dp_sync & 0x7fff);
	m->vsync_end = m->vsync_start + ((t->dp_sync >> 16) & 0x7fff);
	m->flags = ((t->dp_sync & DP_HSYNC_ACTIVE_LOW) != 0 ?
	    DRM_MODE_FLAG_NHSYNC : DRM_MODE_FLAG_PHSYNC) |
	    ((t->dp_sync & DP_VSYNC_ACTIVE_LOW) != 0 ?
	    DRM_MODE_FLAG_NVSYNC : DRM_MODE_FLAG_PVSYNC);
	if (fb->link_rate != 0 && t->rcg_m != 0 &&
	    (t->rcg_cfg & RCG_CFG_MODE_MASK) != 0) {
		n = (~t->rcg_n & 0xffff) + t->rcg_m;
		m->clock = msmfb_pixel_parent(fb) * t->rcg_m / n *
		    msmfb_bus_pixels(fb);
	} else		/* assume 60 Hz */
		m->clock = m->htotal * m->vtotal * 60 / 1000;
	m->width_mm = DRM_MODE_RES_MM(m->hdisplay, 96ul);
	m->height_mm = DRM_MODE_RES_MM(m->vdisplay, 96ul);
	m->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_set_name(m);
}

/* The timing of a mode, as Linux's msm sets it. */
static void
msmfb_compute_timing(struct msmfb *fb, const struct drm_display_mode *m,
    struct msmfb_timing *t)
{
	struct msm_dp_tu_calc_input in;
	struct msm_dp_vc_tu_mapping_table tu;
	u_int bus, hsw, hbp, width, vsw, vbp, hperiod, vperiod, hstart;
	bool nh, nv;

	nh = (m->flags & DRM_MODE_FLAG_NHSYNC) != 0;
	nv = (m->flags & DRM_MODE_FLAG_NVSYNC) != 0;
	*t = fb->fw_timing;

	/* The pixel clock is the interface's, for as many as its bus. */
	bus = msmfb_bus_pixels(fb);
	msm_fbsd_dp_calc_pixel_mnd(msmfb_pixel_parent(fb), m->clock / bus,
	    &t->rcg_m, &t->rcg_n, &t->rcg_d);
	t->rcg_cfg &= ~RCG_CFG_MODE_MASK;
	if (t->rcg_m != 0)
		t->rcg_cfg |= RCG_CFG_MODE_DUAL_EDGE;

	/* The stream, as msm_dp_ctrl_on_stream() sets it up. */
	t->dp_total = m->vtotal << 16 | m->htotal;
	t->dp_start = (m->vtotal - m->vsync_start) << 16 |
	    (m->htotal - m->hsync_start);
	t->dp_sync = (m->vsync_end - m->vsync_start) << 16 |
	    (m->hsync_end - m->hsync_start) |
	    (nh ? DP_HSYNC_ACTIVE_LOW : 0) | (nv ? DP_VSYNC_ACTIVE_LOW : 0);
	t->dp_active = m->vdisplay << 16 | m->hdisplay;
	msm_fbsd_dp_calc_msa(fb->link_rate, m->clock, &t->dp_mvid,
	    &t->dp_nvid);
	memset(&in, 0, sizeof(in));
	in.lclk = fb->link_rate / 1000;
	in.pclk_khz = m->clock;
	in.hactive = m->hdisplay;
	in.hporch = m->htotal - m->hdisplay;
	in.nlanes = fb->lanes;
	in.bpp = 24;
	in.pixel_enc = 444;
	in.compress_ratio = 100;
	msm_fbsd_dp_calc_tu(&in, &tu);
	t->dp_tu = tu.tu_size_minus1;
	t->dp_vb = tu.valid_boundary_link | tu.delay_start_link << 16;
	t->dp_vb2 = (tu.boundary_moderation_en ? 1 : 0) |
	    tu.valid_lower_boundary_link << 1 | tu.upper_boundary_count << 16 |
	    tu.lower_boundary_count << 20;

	t->size = m->vdisplay << 16 | m->hdisplay;

	/*
	 * The interface, as dpu_hw_intf_setup_timing_engine() sets it up for
	 * DP: with the front porches moved to the back porches, and across in
	 * units of what its bus carries.
	 */
	hsw = (m->hsync_end - m->hsync_start) / bus;
	hbp = (m->htotal - m->hsync_end + m->hsync_start - m->hdisplay) / bus;
	width = m->hdisplay / bus;
	vsw = m->vsync_end - m->vsync_start;
	vbp = m->vtotal - m->vsync_end + m->vsync_start - m->vdisplay;
	hperiod = hsw + hbp + width;
	vperiod = vsw + vbp + m->vdisplay;
	hstart = hsw + hbp;
	t->intf_hsync = hperiod << 16 | hsw;
	t->intf_vperiod = vperiod * hperiod;
	t->intf_vpulse = vsw * hperiod;
	t->intf_disp_hctl = (hstart + width - 1) << 16 | hstart;
	t->intf_disp_vstart = (vsw + vbp) * hperiod + hstart;
	t->intf_disp_vend = vperiod * hperiod - 1;
	t->intf_act_hctl = t->intf_disp_hctl;
	t->intf_act_vstart = t->intf_disp_vstart;
	t->intf_act_vend = t->intf_act_vstart + m->vdisplay * hperiod - 1;
	t->intf_polarity = (nv ? 2 : 0) | (nh ? 1 : 0);
	t->intf_config = (t->intf_config & ~INTF_CFG_PROG_FETCH) |
	    INTF_CFG_ACTIVE_H_EN | INTF_CFG_ACTIVE_V_EN;
	t->intf_data_hctl = t->intf_disp_hctl;
	t->intf_act_data_hctl = 0;
}

/*
 * Stop the stream, as msm disables its output: the link idles, at the end of
 * a frame of the stream; the interface stops, at its next vsync; the link
 * goes off.  The PHY and the link clocks stay up.  With sink_lock held.
 */
static void
msmfb_stream_off(struct msmfb *fb)
{
	const struct msm_fbsd_disp *d = fb->disp;
	u_int i, frame_ms;

	frame_ms = 1000 / max(drm_mode_vrefresh(&fb->hw_mode), 10) + 1;
	msmfb_dp_write(fb, DP_INTR_STATUS2, msmfb_dp_read(fb, DP_INTR_STATUS2) |
	    DP_INTR_IDLE_PATTERN_SENT << 1);
	msmfb_dp_write(fb, DP_STATE_CTRL, DP_STATE_PUSH_IDLE);
	for (i = 0; i < 3 * frame_ms && (msmfb_dp_read(fb, DP_INTR_STATUS2) &
	    DP_INTR_IDLE_PATTERN_SENT) == 0; i++)
		usleep_range(1000, 1100);
	if (i == 3 * frame_ms)
		dev_warn(fb->drm.dev, "the link didn't idle\n");
	msmfb_dp_write(fb, DP_INTR_STATUS2, msmfb_dp_read(fb, DP_INTR_STATUS2) |
	    DP_INTR_IDLE_PATTERN_SENT << 1);
	msmfb_write(fb, d->intf + INTF_TIMING_ENGINE_EN, 0);
	msleep(2 * frame_ms);
	msmfb_dp_write(fb, DP_MAINLINK_CTRL, msmfb_dp_read(fb, DP_MAINLINK_CTRL) &
	    ~DP_MAINLINK_ENABLE);
}

/* Set a timing, with the stream stopped. */
static void
msmfb_load_timing(struct msmfb *fb, const struct msmfb_timing *t)
{
	void __iomem *rcg = fb->mdss + fb->disp->dp_pixel_rcg;
	u_int i;

	for (i = 0; i < nitems(msmfb_timing_regs); i++)
		writel(*msmfb_treg_field(__DECONST(struct msmfb_timing *, t),
		    i), msmfb_block(fb, msmfb_timing_regs[i].block) +
		    msmfb_timing_regs[i].reg);
	writel(readl(rcg + RCG_CMD) | RCG_CMD_UPDATE, rcg + RCG_CMD);
	for (i = 0; i < 500 && (readl(rcg + RCG_CMD) & RCG_CMD_UPDATE) != 0;
	    i++)
		udelay(1);
	if (i == 500)
		dev_warn(fb->drm.dev, "the pixel clock didn't change\n");
}

/*
 * Start the stream, as msm enables its output: the link on, reset, as
 * msm_dp_catalog_ctrl_mainlink_ctrl() has it, and trained, which leaves it
 * sending the stream; then the interface with its settings flushed in.
 * With sink_lock held.
 */
static void
msmfb_stream_on(struct msmfb *fb)
{
	const struct msm_fbsd_disp *d = fb->disp;
	unsigned long flags;
	u32 ml;

	ml = msmfb_dp_read(fb, DP_MAINLINK_CTRL) &
	    ~(DP_MAINLINK_ENABLE | DP_MAINLINK_RESET);
	msmfb_dp_write(fb, DP_MAINLINK_CTRL, ml);
	msmfb_dp_write(fb, DP_MAINLINK_CTRL, ml | DP_MAINLINK_RESET);
	msmfb_dp_write(fb, DP_MAINLINK_CTRL, ml);
	msmfb_dp_write(fb, DP_MAINLINK_CTRL, ml | DP_MAINLINK_ENABLE |
	    DP_MAINLINK_FB_BOUNDARY_SEL);
	if (!fb->connected || msmfb_link_train(fb) != 0)
		msmfb_dp_write(fb, DP_STATE_CTRL, DP_STATE_SEND_VIDEO);
	spin_lock_irqsave(&fb->lock, flags);
	msmfb_write(fb, d->ctl + CTL_INTF_FLUSH, d->ctl_flush_intf);
	msmfb_write(fb, d->ctl + CTL_FLUSH, d->ctl_flush_sspp |
	    d->ctl_flush_lm | CTL_FLUSH_CTL | CTL_FLUSH_INTF);
	spin_unlock_irqrestore(&fb->lock, flags);
	msmfb_write(fb, d->intf + INTF_TIMING_ENGINE_EN, 1);
}

/* Whether the sink holds the link: its lanes locked and aligned. */
static bool
msmfb_link_ok(struct msmfb *fb)
{
	u8 st[DP_LINK_STATUS_SIZE];

	return (msmfb_aux(fb, false, true, DP_LANE0_1_STATUS, st,
	    sizeof(st)) == 0 && drm_dp_channel_eq_ok(st, fb->lanes));
}

/*
 * Change the stream over, as msm's DPU encoder and DP bridge do when they are
 * disabled and enabled again; with the output off, only set its timing.
 */
static void
msmfb_set_mode(struct msmfb *fb, const struct drm_display_mode *mode)
{
	struct msmfb_timing t;

	mutex_lock(&fb->mode_lock);
	if (msmfb_mode_same(&fb->hw_mode, mode)) {
		mutex_unlock(&fb->mode_lock);
		return;
	}
	if (msmfb_mode_same(&fb->fw_mode, mode))
		t = fb->fw_timing;
	else
		msmfb_compute_timing(fb, mode, &t);
	mutex_lock(&fb->sink_lock);
	if (fb->output_off)
		msmfb_load_timing(fb, &t);
	else {
		msmfb_stream_off(fb);
		msmfb_load_timing(fb, &t);
		msmfb_stream_on(fb);
		if (fb->connected && !msmfb_link_ok(fb))
			dev_info(fb->drm.dev,
			    "link not locked after mode change\n");
	}
	mutex_unlock(&fb->sink_lock);
	drm_mode_copy(&fb->hw_mode, mode);
	mutex_unlock(&fb->mode_lock);
	dev_info(fb->drm.dev, "mode " DRM_MODE_FMT "\n", DRM_MODE_ARG(mode));
}

/*
 * Turn the output off or on (DPMS): with no stream, the monitor goes to
 * sleep.
 */
static void
msmfb_output(struct msmfb *fb, bool on)
{
	mutex_lock(&fb->mode_lock);
	if (fb->output_off == !on) {
		mutex_unlock(&fb->mode_lock);
		return;
	}
	mutex_lock(&fb->sink_lock);
	if (on)
		msmfb_stream_on(fb);
	else
		msmfb_stream_off(fb);
	WRITE_ONCE(fb->output_off, !on);
	mutex_unlock(&fb->sink_lock);
	mutex_unlock(&fb->mode_lock);
	dev_info(fb->drm.dev, "output %s\n", on ? "on" : "off");
}

/* Show the console: the firmware's mode and framebuffer, the output on. */
static void
msmfb_show_console(struct msmfb *fb)
{
	unsigned long flags;

	msmfb_set_mode(fb, &fb->fw_mode);
	msmfb_output(fb, true);
	spin_lock_irqsave(&fb->lock, flags);
	msmfb_scanout(fb, fb->console_addr, fb->console_stride);
	spin_unlock_irqrestore(&fb->lock, flags);
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
		for (i = 0; i < bo->npages; i++)
			if (vm_page_unwire_noq(bo->pages[i]))
				vm_page_free(bo->pages[i]);
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

/*
 * Physically contiguous, zeroed pages below 4 GB, for the pipe's 32-bit
 * addresses: exactly as many as asked, where alloc_pages() would round up
 * to a power of two, and making room for them by reclaiming, as
 * kmem_alloc_contig() does, when memory has fragmented.
 */
static vm_page_t
msmfb_alloc_contig(u_long npages)
{
	vm_page_t m;
	int tries;

	for (tries = 0; tries < 4; tries++) {
		m = vm_page_alloc_noobj_contig(VM_ALLOC_WIRED | VM_ALLOC_ZERO,
		    npages, 0, BUS_SPACE_MAXADDR_32BIT, PAGE_SIZE, 0,
		    VM_MEMATTR_DEFAULT);
		if (m != NULL)
			return (m);
		if (vm_page_reclaim_contig(VM_ALLOC_WIRED, npages, 0,
		    BUS_SPACE_MAXADDR_32BIT, PAGE_SIZE, 0) == ENOMEM)
			vm_wait(NULL);
	}
	return (NULL);
}

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

	bo->npages = size >> PAGE_SHIFT;
	bo->pages = kvcalloc(bo->npages, sizeof(*bo->pages), GFP_KERNEL);
	if (bo->pages == NULL)
		goto fail;
	bo->page = msmfb_alloc_contig(bo->npages);
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
	irqreturn_t ret = IRQ_NONE;
	u32 mdss, status;

	mdss = readl(fb->mdss + MDSS_HW_INTR_STATUS);
	if ((mdss & d->mdss_intr_dp) != 0) {
		/* Hotplug: acknowledge, and look at the sink from a task. */
		status = msmfb_dp_read(fb, DP_HPD_INT_STATUS) &
		    msmfb_dp_read(fb, DP_HPD_INT_MASK) & DP_HPD_EVENTS;
		msmfb_dp_write(fb, DP_HPD_INT_ACK, status);
		if (status != 0)
			schedule_work(&fb->hotplug_work);
		ret = IRQ_HANDLED;
	}
	status = msmfb_read(fb, MDP_INTR_STATUS) & msmfb_read(fb, MDP_INTR_EN);
	if ((status & d->intr_vsync) == 0)
		return (ret);
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

	if (msmfb_mode_same(mode, &fb->fw_mode))
		return (MODE_OK);
	/* Others need the link's rate, for the pixel clock. */
	if (fb->link_rate == 0)
		return (MODE_BAD);
	if ((mode->flags & DRM_MODE_FLAG_INTERLACE) != 0)
		return (MODE_NO_INTERLACE);
	if ((mode->flags & DRM_MODE_FLAG_DBLSCAN) != 0)
		return (MODE_NO_DBLESCAN);
	if (mode->hdisplay > MSMFB_MAX_WIDTH)
		return (MODE_BAD_HVALUE);
	if (msmfb_bus_pixels(fb) == 2 && ((mode->hdisplay | mode->hsync_start |
	    mode->hsync_end | mode->htotal) & 1) != 0)
		return (MODE_H_ILLEGAL);
	if (mode->clock > MSMFB_MAX_PIXEL_KHZ ||
	    (uint64_t)mode->clock * 24 > (uint64_t)fb->link_rate * 8 * fb->lanes)
		return (MODE_CLOCK_HIGH);
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

/*
 * A new mode goes on before the plane shows a buffer of its size.  It goes
 * on here, not in atomic_enable, for the firmware's mode that the console
 * gets back behind the atomic state's back.
 */
static void
msmfb_crtc_atomic_begin(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct msmfb *fb = container_of(crtc, struct msmfb, crtc);
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);

	if (new->active)
		msmfb_set_mode(fb, &new->adjusted_mode);
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
	msmfb_output(container_of(crtc, struct msmfb, crtc), true);
	drm_crtc_vblank_on(crtc);
}

static void
msmfb_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct msmfb *fb = container_of(crtc, struct msmfb, crtc);
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);

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
	/* Off, rather than on in another mode. */
	if (!new->active)
		msmfb_output(fb, false);
}

static const struct drm_crtc_helper_funcs msmfb_crtc_helper_funcs = {
	.mode_valid = msmfb_crtc_mode_valid,
	.atomic_check = msmfb_crtc_atomic_check,
	.atomic_begin = msmfb_crtc_atomic_begin,
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
	int n;

	/* The monitor's modes, or without its EDID the firmware's. */
	mutex_lock(&fb->sink_lock);
	drm_edid_connector_update(connector, fb->edid);
	n = 0;
	if (fb->connected) {
		n = drm_edid_connector_add_modes(connector);
		if (n == 0)
			n = drm_connector_helper_get_modes_fixed(connector,
			    &fb->fw_mode);
	}
	mutex_unlock(&fb->sink_lock);
	return (n);
}

static enum drm_connector_status
msmfb_connector_detect(struct drm_connector *connector, bool force)
{
	struct msmfb *fb = container_of(connector, struct msmfb, connector);

	return (READ_ONCE(fb->connected) ? connector_status_connected :
	    connector_status_disconnected);
}

static const struct drm_connector_helper_funcs msmfb_connector_helper_funcs = {
	.get_modes = msmfb_connector_get_modes,
};

static const struct drm_connector_funcs msmfb_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.detect = msmfb_connector_detect,
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

	/*
	 * Show the console again: the client's buffers may go away, and
	 * nothing else will point the pipe back, or turn the output on.
	 */
	msmfb_show_console(fb);
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

	error = drmm_mode_config_init(drm);
	if (error != 0)
		return (error);
	drm->mode_config.max_width = MSMFB_MAX_WIDTH;
	drm->mode_config.max_height = MSMFB_MAX_WIDTH;
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
	fb->connector.polled = DRM_CONNECTOR_POLL_HPD;
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
	fb->mdss = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(fb->mdss))
		return (PTR_ERR(fb->mdss));
	fb->dp = fb->mdss + d->dp;
	fb->mdp = fb->mdss + d->mdp;
	mutex_init(&fb->sink_lock);
	mutex_init(&fb->mode_lock);
	INIT_WORK(&fb->hotplug_work, msmfb_hotplug_work);

	/*
	 * Take over only the pipeline we know: an interface that is running
	 * and a source pipe that scans out something.
	 */
	fb->console_addr = msmfb_read(fb, d->sspp + SSPP_SRC0_ADDR);
	fb->console_stride = msmfb_read(fb, d->sspp + SSPP_SRC_YSTRIDE0) &
	    0xffff;
	size = msmfb_read(fb, d->sspp + SSPP_SRC_SIZE);
	if ((msmfb_read(fb, d->intf + INTF_TIMING_ENGINE_EN) & 1) == 0 ||
	    fb->console_addr == 0 || (size & 0xffff) == 0 || size >> 16 == 0) {
		dev_info(&pdev->dev, "the firmware left no display running\n");
		return (-ENODEV);
	}

	msmfb_probe_sink(fb);
	(void)msmfb_update_sink(fb);
	msmfb_read_timing(fb, &fb->fw_timing);
	if (fb->fw_timing.dp_active != size) {
		dev_warn(&pdev->dev, "the pipe and the stream differ in size; "
		    "keeping the firmware's mode\n");
		fb->link_rate = 0;
	}
	msmfb_timing_mode(fb, &fb->fw_timing, &fb->fw_mode);
	drm_mode_copy(&fb->hw_mode, &fb->fw_mode);
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
	/* Hotplug events, less the one the firmware's plug left pending. */
	msmfb_dp_write(fb, DP_HPD_INT_ACK, DP_HPD_EVENTS);
	msmfb_dp_write(fb, DP_HPD_INT_MASK, DP_HPD_EVENTS);
	dev_info(&pdev->dev, "firmware mode " DRM_MODE_FMT ", scanning out "
	    "%#x\n", DRM_MODE_ARG(&fb->fw_mode), fb->console_addr);
	return (0);
}

static void
msmfb_remove(struct platform_device *pdev)
{
	struct msmfb *fb = platform_get_drvdata(pdev);

	msmfb_dp_write(fb, DP_HPD_INT_MASK, 0);
	cancel_work_sync(&fb->hotplug_work);
	drm_dev_unplug(&fb->drm);
	drm_atomic_helper_shutdown(&fb->drm);
	msmfb_show_console(fb);
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
