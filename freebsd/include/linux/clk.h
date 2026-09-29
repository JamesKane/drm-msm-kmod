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
 * Clocks.  The GPU's clocks are managed as a whole with its CX power domain
 * (qcom_gpucc(4)), and the GPU core clock by the GMU, so the clocks msm asks
 * for by name are placeholders.
 */
#ifndef _MSM_FREEBSD_LINUX_CLK_H_
#define	_MSM_FREEBSD_LINUX_CLK_H_

#include <linux/types.h>
#include <linux/err.h>

struct device;
struct clk;

struct clk_bulk_data {
	const char	*id;
	struct clk	*clk;
};

struct clk	*devm_clk_get(struct device *dev, const char *id);
int	devm_clk_bulk_get_all(struct device *dev, struct clk_bulk_data **clks);
int	clk_prepare_enable(struct clk *clk);
void	clk_disable_unprepare(struct clk *clk);
int	clk_set_rate(struct clk *clk, unsigned long rate);
unsigned long clk_get_rate(struct clk *clk);

static inline int
clk_bulk_prepare_enable(int num, const struct clk_bulk_data *clks)
{
	int i, error;

	for (i = 0; i < num; i++) {
		error = clk_prepare_enable(clks[i].clk);
		if (error != 0) {
			while (i-- > 0)
				clk_disable_unprepare(clks[i].clk);
			return (error);
		}
	}
	return (0);
}

static inline void
clk_bulk_disable_unprepare(int num, const struct clk_bulk_data *clks)
{
	while (num-- > 0)
		clk_disable_unprepare(clks[num].clk);
}

#endif
