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
  - `msm_freebsd_iommu.c`: IOMMU domains on qcom_smmu.
  - `msm_freebsd_misc.c`: Command DB, MDT firmware loader, GEM pages,
    stubs for the parts not built.
  - `include/`: Linux headers LinuxKPI lacks or stubs, searched first, and
    `msm_freebsd_compat.h`, included into every file.

Changes to the Linux sources are marked `__FreeBSD__`, or use Linux's own
accessors (`dev_of_node()`, `dev_get_platdata()`).

Generic pieces that belong in LinuxKPI or drm-kmod eventually:
absolute-mode hrtimers, `readl_poll_timeout`, 64-bit `SZ_*G` (LinuxKPI's
overflow), `idr_alloc_u32`, platform devices, `drm_gem_get_pages()`.

Build (cross or native): `make DRMKMOD=/path/to/drm-kmod SYSDIR=/usr/src/sys`.

Status: work in progress; GPU only, display stays on the firmware
framebuffer (sysfbdrm).
