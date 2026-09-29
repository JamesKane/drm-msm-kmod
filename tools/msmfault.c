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
 * GPU address space isolation test.
 *   msmfault hold         map two buffers, fill the second, wait 8 s and
 *                         check that it is unchanged
 *   msmfault write IOVA   from this process, make the GPU write to IOVA, and
 *                         to a buffer of its own
 *   msmfault hang         make the GPU wait for a value that never comes
 */
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <err.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <drm.h>
#include <msm_drm.h>

#define	PATTERN	0xaaaaaaaau
#define	VALUE	0x12345678u

static int fd;

struct bo {
	uint32_t	handle;
	uint64_t	iova;
	uint32_t	*map;
};

static void
bo_new(struct bo *b)
{
	struct drm_msm_gem_new req = { .size = 4096, .flags = MSM_BO_WC };
	struct drm_msm_gem_info info = { 0 };

	if (ioctl(fd, DRM_IOCTL_MSM_GEM_NEW, &req) != 0)
		err(1, "GEM_NEW");
	b->handle = req.handle;
	info.handle = b->handle;
	info.info = MSM_INFO_GET_IOVA;
	if (ioctl(fd, DRM_IOCTL_MSM_GEM_INFO, &info) != 0)
		err(1, "GEM_INFO iova");
	b->iova = info.value;
	info.info = MSM_INFO_GET_OFFSET;
	if (ioctl(fd, DRM_IOCTL_MSM_GEM_INFO, &info) != 0)
		err(1, "GEM_INFO offset");
	b->map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    info.value);
	if (b->map == MAP_FAILED)
		err(1, "mmap");
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

static int
mem_write(uint32_t *p, uint64_t iova, uint32_t v)
{
	p[0] = pkt7(0x3d, 3);			/* CP_MEM_WRITE */
	p[1] = (uint32_t)iova;
	p[2] = (uint32_t)(iova >> 32);
	p[3] = v;
	return (4);
}

static void
hold(void)
{
	struct bo a, b;

	bo_new(&a);
	bo_new(&b);
	b.map[0] = PATTERN;
	printf("holder: buffers at %#jx and %#jx, the second holds %#x\n",
	    (uintmax_t)a.iova, (uintmax_t)b.iova, b.map[0]);
	fflush(stdout);
	sleep(8);
	printf("holder: the second buffer now holds %#x: %s\n", b.map[0],
	    b.map[0] == PATTERN ? "untouched" : "OVERWRITTEN");
}

static int
wait_forever(uint32_t *p, uint64_t iova)
{
	p[0] = pkt7(0x3c, 6);			/* CP_WAIT_REG_MEM */
	p[1] = 3 | 1 << 4;			/* equal, poll memory */
	p[2] = (uint32_t)iova;
	p[3] = (uint32_t)(iova >> 32);
	p[4] = 1;				/* never written */
	p[5] = 0xffffffff;
	p[6] = 0;
	return (7);
}

static void
write_to(uint64_t target, bool hang)
{
	struct drm_msm_submitqueue q = { 0 };
	struct drm_msm_gem_submit_bo sbo = { 0 };
	struct drm_msm_gem_submit_cmd cmd = { 0 };
	struct drm_msm_gem_submit sub = { 0 };
	struct drm_msm_wait_fence wait = { 0 };
	struct timespec ts;
	struct bo c;
	int n;

	bo_new(&c);
	c.map[0x800 / 4] = 0;
	n = 0;
	if (hang) {
		c.map[0x900 / 4] = 0;
		n += wait_forever(c.map + n, c.iova + 0x900);
		printf("hang: waiting on %#jx\n", (uintmax_t)c.iova + 0x900);
	} else {
		n += mem_write(c.map + n, target, VALUE);
		printf("writer: own buffer at %#jx; writing %#x to %#jx and "
		    "to it\n", (uintmax_t)c.iova, VALUE, (uintmax_t)target);
	}
	n += mem_write(c.map + n, c.iova + 0x800, VALUE);

	if (ioctl(fd, DRM_IOCTL_MSM_SUBMITQUEUE_NEW, &q) != 0)
		err(1, "SUBMITQUEUE_NEW");
	sbo.flags = MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE;
	sbo.handle = c.handle;
	cmd.type = MSM_SUBMIT_CMD_BUF;
	cmd.size = n * 4;
	sub.flags = MSM_PIPE_3D0;
	sub.nr_bos = 1;
	sub.bos = (uintptr_t)&sbo;
	sub.nr_cmds = 1;
	sub.cmds = (uintptr_t)&cmd;
	sub.queueid = q.id;
	if (ioctl(fd, DRM_IOCTL_MSM_GEM_SUBMIT, &sub) != 0)
		err(1, "GEM_SUBMIT");
	clock_gettime(CLOCK_MONOTONIC, &ts);
	ts.tv_sec += hang ? 10 : 3;
	wait.fence = sub.fence;
	wait.queueid = q.id;
	wait.timeout.tv_sec = ts.tv_sec;
	wait.timeout.tv_nsec = ts.tv_nsec;
	if (ioctl(fd, DRM_IOCTL_MSM_WAIT_FENCE, &wait) != 0)
		err(1, "WAIT_FENCE");
	printf("%s: fence signalled; own buffer holds %#x\n",
	    hang ? "hang" : "writer", c.map[0x800 / 4]);
}

int
main(int argc, char **argv)
{
	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		err(1, "open");
	if (argc == 2 && strcmp(argv[1], "hold") == 0)
		hold();
	else if (argc == 3 && strcmp(argv[1], "write") == 0)
		write_to(strtoull(argv[2], NULL, 0), false);
	else if (argc == 2 && strcmp(argv[1], "hang") == 0)
		write_to(0, true);
	else
		errx(1, "usage: msmfault hold | write iova | hang");
	return (0);
}
