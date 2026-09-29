# drm-msm-kmod

The Linux msm DRM driver (Qualcomm Adreno GPU) for FreeBSD, built against
[drm-kmod](https://github.com/freebsd/drm-kmod)'s DRM core and LinuxKPI.

- `drivers/gpu/drm/msm/`: the driver from Linux v6.13, with its generated
  register headers.  GPL-2.0-only.
- `msm/`: the module build.
- `freebsd/` (to come): FreeBSD glue, which attaches to the ACPI GPU device
  and provides the Linux interfaces msm uses on top of FreeBSD's Qualcomm
  drivers (qcom_gpucc, qcom_scm, qcom_cmd_db, qcom_smmu).

Build (cross or native): `make DRMKMOD=/path/to/drm-kmod SYSDIR=/usr/src/sys`.

Status: work in progress; GPU only, display stays on the firmware
framebuffer (sysfbdrm).
