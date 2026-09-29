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
 * Smoke test for msm: query the GPU, then submit a few CP_NOP
 * packets and wait for their fence.
 */
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <drm.h>
#include <msm_drm.h>

static int fd;

static uint64_t
param(uint32_t p)
{
	struct drm_msm_param req = { .pipe = MSM_PIPE_3D0, .param = p };

	if (ioctl(fd, DRM_IOCTL_MSM_GET_PARAM, &req) != 0)
		err(1, "GET_PARAM %u", p);
	return (req.value);
}

static unsigned
odd_parity(unsigned v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	v &= 0xf;
	return ((~0x6996 >> v) & 1);
}

static uint32_t
pkt7(unsigned opcode, unsigned cnt)
{
	return (0x70000000 | cnt | odd_parity(cnt) << 15 |
	    (opcode & 0x7f) << 16 | odd_parity(opcode) << 23);
}

int
main(int argc, char **argv)
{
	struct drm_msm_submitqueue q = { 0 };
	struct drm_msm_gem_new bo = { .size = 4096, .flags = MSM_BO_WC };
	struct drm_msm_gem_info info = { 0 };
	struct drm_msm_gem_submit_bo sbo = { 0 };
	struct drm_msm_gem_submit_cmd cmd = { 0 };
	struct drm_msm_gem_submit sub = { 0 };
	struct drm_msm_wait_fence wait = { 0 };
	struct timespec ts;
	uint32_t *p;
	int i, n;

	fd = open(argc > 1 ? argv[1] : "/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		err(1, "open");
	printf("chip id %#jx gpu id %ju gmem %#jx\n",
	    (uintmax_t)param(MSM_PARAM_CHIP_ID),
	    (uintmax_t)param(MSM_PARAM_GPU_ID),
	    (uintmax_t)param(MSM_PARAM_GMEM_SIZE));
	printf("timestamp %ju\n", (uintmax_t)param(MSM_PARAM_TIMESTAMP));
	usleep(10000);
	printf("timestamp %ju (10 ms later)\n",
	    (uintmax_t)param(MSM_PARAM_TIMESTAMP));

	if (ioctl(fd, DRM_IOCTL_MSM_SUBMITQUEUE_NEW, &q) != 0)
		err(1, "SUBMITQUEUE_NEW");
	if (ioctl(fd, DRM_IOCTL_MSM_GEM_NEW, &bo) != 0)
		err(1, "GEM_NEW");
	info.handle = bo.handle;
	info.info = MSM_INFO_GET_OFFSET;
	if (ioctl(fd, DRM_IOCTL_MSM_GEM_INFO, &info) != 0)
		err(1, "GEM_INFO");
	p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    info.value);
	if (p == MAP_FAILED)
		err(1, "mmap");
	n = 8;
	for (i = 0; i < n; i++)
		p[i] = pkt7(0x10, 0);		/* CP_NOP */
	info.info = MSM_INFO_GET_IOVA;
	if (ioctl(fd, DRM_IOCTL_MSM_GEM_INFO, &info) != 0)
		err(1, "GEM_INFO iova");
	printf("cmd bo iova %#jx\n", (uintmax_t)info.value);

	sbo.flags = MSM_SUBMIT_BO_READ;
	sbo.handle = bo.handle;
	cmd.type = MSM_SUBMIT_CMD_BUF;
	cmd.submit_idx = 0;
	cmd.size = n * 4;
	sub.flags = MSM_PIPE_3D0;
	sub.nr_bos = 1;
	sub.bos = (uintptr_t)&sbo;
	sub.nr_cmds = 1;
	sub.cmds = (uintptr_t)&cmd;
	sub.queueid = q.id;
	if (ioctl(fd, DRM_IOCTL_MSM_GEM_SUBMIT, &sub) != 0)
		err(1, "GEM_SUBMIT");
	printf("submitted, fence %u\n", sub.fence);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ts.tv_sec += 10;
	wait.fence = sub.fence;
	wait.queueid = q.id;
	wait.timeout.tv_sec = ts.tv_sec;
	wait.timeout.tv_nsec = ts.tv_nsec;
	if (ioctl(fd, DRM_IOCTL_MSM_WAIT_FENCE, &wait) != 0)
		err(1, "WAIT_FENCE");
	printf("fence %u signalled: the GPU executed the submit\n", sub.fence);
	return (0);
}
