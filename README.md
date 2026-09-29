# drm-msm-kmod

The Linux msm DRM driver (Qualcomm Adreno GPU) for FreeBSD, built against
[drm-kmod](https://github.com/freebsd/drm-kmod)'s DRM core and LinuxKPI.

- `drivers/gpu/drm/msm/`: the driver from Linux v6.13, with its generated
  register headers.  GPL-2.0-only.
- `msm/`: the module build.
- `freebsd/`: FreeBSD glue (BSD-2-Clause), which attaches to the ACPI GPU
  device (QCOM0636) and provides the Linux interfaces msm uses on top of
  FreeBSD's Qualcomm drivers (qcom_gpucc, qcom_scm, qcom_cmd_db, qcom_smmu):
  - `msm_freebsd_bus.c`: the ACPI driver; powers the GPU's CX domain,
    claims its SMMU, and handles interrupts.
  - `msm_freebsd.c`: creates the platform devices msm's drivers attach to
    (GMU, GPU, headless DRM device); request_irq().
  - `msm_freebsd_soc.c`: what Linux's devicetree says about the SoC and ACPI
    does not (nodes, register windows, interrupts, OPPs, SMMU streams).
  - `msm_freebsd_platform.c`: platform devices and drivers, device tree
    lookups, components.
  - `msm_freebsd_power.c`: runtime PM, power domains, clocks, OPP tables.
  - `msm_freebsd_iommu.c`: IOMMU domains on qcom_smmu: split page tables
    and adreno_smmu_priv for per-process GPU address spaces, io-pgtable,
    and fault reporting.
  - `msm_freebsd_misc.c`: Command DB, MDT firmware loader, GEM pages,
    stubs for the parts not built.
  - `include/`: Linux headers LinuxKPI lacks or stubs, searched first, and
    `msm_freebsd_compat.h`, included into every file.
- `tools/`: test programs (BSD-2-Clause), built with `make` in `tools/`:
  - `msmtest`: queries the GPU and runs a submission of CP_NOPs.
  - `msmfault`: `hold` and `write IOVA`, run as two processes, check that
    one process's GPU work cannot write to another's memory (the write
    takes an SMMU fault); `hang` submits work that never completes, to
    exercise hang detection and recovery.
  - `egltest`: offscreen OpenGL ES through GBM and EGL; checks rendered
    pixels.
  - `vktest`: lists Vulkan devices and creates a device on each.

Changes to the Linux sources are marked `__FreeBSD__`, or use Linux's own
accessors (`dev_of_node()`, `dev_get_platdata()`).

Generic pieces that belong in LinuxKPI or drm-kmod eventually:
absolute-mode hrtimers, `readl_poll_timeout`, 64-bit `SZ_*G` (LinuxKPI's
overflow), `idr_alloc_u32`, platform devices, `drm_gem_get_pages()`.

## Licensing

Each file states its license with an SPDX identifier or in its own text;
the license texts are in `LICENSES/`:

- GPL-2.0 (`GPL-2.0-only`, and a few `GPL-2.0-or-later`): the msm driver
  and the Linux headers imported with it.
- MIT: the Adreno register descriptions in
  `drivers/gpu/drm/msm/registers/`, the headers generated from them in
  `drivers/gpu/drm/msm/generated/`, and `gen_header.py`.
- BSD-2-Clause: the FreeBSD glue in `freebsd/`, `msm/`, and `tools/`.

The module built from these, msm.ko, is covered by the GPL-2.0.

Build (cross or native): `make DRMKMOD=/path/to/drm-kmod SYSDIR=/usr/src/sys`.

Status: work in progress.  On a Radxa Dragon Q8B (SC8280XP, Adreno 690)
the GMU boots, the zap shader loads, and Mesa's freedreno and Turnip run
OpenGL ES 3.2 and Vulkan 1.3, with an address space per process, SMMU
fault reporting and hang recovery.  GPU only: display stays on the firmware
framebuffer (sysfbdrm in drm-kmod).  The CX power domain stays on while msm
is attached.
