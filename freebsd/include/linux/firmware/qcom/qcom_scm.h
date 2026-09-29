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
 * The SCM calls msm makes, on top of qcom_scm(4).  Shadows Linux's header,
 * whose other functions share names with qcom_scm(4)'s but not signatures.
 */
#ifndef _MSM_FREEBSD_LINUX_QCOM_SCM_H_
#define	_MSM_FREEBSD_LINUX_QCOM_SCM_H_

#include <linux/types.h>
#include <linux/errno.h>

#include <dev/qcom_scm/qcom_scm.h>

#define	QCOM_SCM_GPU_ALWAYS_EN_REQ	(1 << 0)
#define	QCOM_SCM_GPU_TSENSE_EN_REQ	(1 << 1)

static inline bool
qcom_scm_is_available(void)
{
	return (qcom_scm_available());
}

/* qcom_scm(4) returns errno values; Linux callers expect them negated. */
#define	qcom_scm_pas_auth_and_reset(id)					\
	(-(qcom_scm_pas_auth_and_reset)(id))
#define	qcom_scm_set_gpu_smmu_aperture(cb)				\
	(-(qcom_scm_set_gpu_smmu_aperture)(cb))
#define	qcom_scm_set_remote_state(state, id)				\
	(-(qcom_scm_set_remote_state)((state), (id)))

/* Only A7xx GPUs need it. */
static inline int
qcom_scm_gpu_init_regs(u32 gpu_req __unused)
{
	return (-EOPNOTSUPP);
}

#endif
