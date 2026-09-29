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
 * What the devicetree tells Linux about the SC8280XP GPU, and ACPI does not:
 * the nodes msm looks up, register windows and interrupts by name, OPP
 * tables, and SMMU stream IDs.  From Linux's sc8280xp.dtsi.
 */

#include "msm_freebsd.h"

static const char *const sc8280xp_machine_compat[] = { "qcom,sc8280xp", NULL };
static const char *const sc8280xp_gpu_compat[] = {
	"qcom,adreno-690.0", "qcom,adreno", NULL
};
static const char *const sc8280xp_gmu_compat[] = {
	"qcom,adreno-gmu-690.0", "qcom,adreno-gmu", NULL
};
static const char *const sc8280xp_smmu_compat[] = {
	"qcom,sc8280xp-smmu-500", "qcom,adreno-smmu", "qcom,smmu-500",
	"arm,mmu-500", NULL
};

static const struct of_prop sc8280xp_gpu_props[] = {
	{ .name = "qcom,gmu", .phandle = "gmu" },
	{ .name = "iommus", .phandle = "gpu-smmu" },
	{ .name = NULL }
};

static const struct of_prop sc8280xp_zap_props[] = {
	{ .name = "memory-region", .phandle = "gpu-mem" },
	{ .name = "firmware-name",
	  .str = "qcom/sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn" },
	{ .name = NULL }
};

static struct device_node sc8280xp_machine = {
	.name = "machine",
	.compatible = sc8280xp_machine_compat,
};

static struct device_node sc8280xp_nodes[] = {
	{ .name = "gpu", .compatible = sc8280xp_gpu_compat,
	  .props = sc8280xp_gpu_props },
	{ .name = "zap-shader", .parent_name = "gpu",
	  .props = sc8280xp_zap_props },
	{ .name = "gpu-mem",
	  .reg = { .start = 0x8bf00000, .end = 0x8bf01fff,
	      .flags = IORESOURCE_MEM } },
	{ .name = "gmu", .compatible = sc8280xp_gmu_compat },
	{ .name = "gpu-smmu", .compatible = sc8280xp_smmu_compat },
	{ .name = NULL }
};

static const struct msm_fbsd_res sc8280xp_gpu_res[] = {
	{ "kgsl_3d0_reg_memory", 0x3d00000, 0x40000, 0 },
	{ "cx_mem", 0x3d9e000, 0x1000, 0 },
	{ "cx_dbgc", 0x3d61000, 0x800, 0 },
	{ "gpu", 332, 0, 3 },		/* SPI 300; ACPI GPU0 interrupt 3 */
	{ NULL }
};

static const struct msm_fbsd_res sc8280xp_gmu_res[] = {
	{ "gmu", 0x3d6a000, 0x34000, 0 },
	{ "rscc", 0x3de0000, 0x10000, 0 },
	{ "gmu_pdc", 0xb290000, 0x10000, 0 },
	{ "hfi", 336, 0, -1 },		/* SPI 304; not in ACPI */
	{ "gmu", 337, 0, -1 },		/* SPI 305; not in ACPI */
	{ NULL }
};

static const struct msm_fbsd_opp sc8280xp_gpu_opps[] = {
	{ 270000000, 0x040, 450000 },
	{ 410000000, 0x080, 1555000 },
	{ 500000000, 0x0c0, 1555000 },
	{ 547000000, 0x0e0, 1555000 },
	{ 606000000, 0x100, 2736000 },
	{ 640000000, 0x140, 2736000 },
	{ 655000000, 0x180, 2736000 },
	{ 690000000, 0x1a0, 2736000 },
	{ 0 }
};

static const struct msm_fbsd_opp sc8280xp_gmu_opps[] = {
	{ 200000000, 0x30, 0 },
	{ 500000000, 0x80, 0 },
	{ 0 }
};

static const struct msm_fbsd_pdev_desc sc8280xp_pdevs[] = {
	/*
	 * The headless DRM device; its component is the GPU.  It sits under
	 * the GPU, as a display would under MDSS, so that msm_use_mmu() sees
	 * the GPU's SMMU and buffers use it instead of a VRAM carveout.
	 */
	{ .name = "msm", .parent = "adreno" },
	{ .name = "adreno", .node = "gpu", .res = sc8280xp_gpu_res,
	  .opps = sc8280xp_gpu_opps,
	  .sid = { 0x0, 0x1 }, .sid_mask = { 0xc00, 0xc00 }, .nsids = 2 },
	{ .name = "gmu", .node = "gmu", .res = sc8280xp_gmu_res,
	  .opps = sc8280xp_gmu_opps,
	  /*
	   * The mask is the board firmware devicetree's.  A narrower one
	   * leaves some GMU streams unmatched, and the resulting global
	   * SMMU fault resets the SoC.
	   */
	  .sid = { 0x5 }, .sid_mask = { 0xc00 }, .nsids = 1 },
	{ .name = NULL }
};

const struct msm_fbsd_soc msm_fbsd_sc8280xp = {
	.pep_hid = "QCOM0617",
	.machine = &sc8280xp_machine,
	.nodes = sc8280xp_nodes,
	.pdevs = sc8280xp_pdevs,
};
