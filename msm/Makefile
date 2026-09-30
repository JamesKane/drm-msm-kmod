# The msm DRM driver (Qualcomm Adreno GPU), built against drm-kmod.
# GPU only for now: display stays with the firmware framebuffer.

DRMKMOD?=	${.CURDIR:H:H}/drm-kmod
.include "${DRMKMOD}/kconfig.mk"

MSMDIR=	${.CURDIR:H}/drivers/gpu/drm/msm

.PATH:	${MSMDIR} ${MSMDIR}/adreno ${.CURDIR:H}/freebsd

KMOD=	msm

SRCS=	msm_drv.c \
	msm_fence.c \
	msm_gem.c \
	msm_gem_prime.c \
	msm_gem_shrinker.c \
	msm_gem_submit.c \
	msm_gem_vma.c \
	msm_gpu.c \
	msm_gpu_devfreq.c \
	msm_io_utils.c \
	msm_iommu.c \
	msm_ringbuffer.c \
	msm_submitqueue.c

SRCS+=	adreno_device.c \
	adreno_gpu.c \
	a6xx_catalog.c \
	a6xx_gmu.c \
	a6xx_gpu.c \
	a6xx_hfi.c \
	a6xx_preempt.c

SRCS+=	msm_freebsd.c \
	msm_freebsd_bus.c \
	msm_freebsd_devfreq.c \
	msm_freebsd_dp_calc.c \
	msm_freebsd_fb.c \
	msm_freebsd_iommu.c \
	msm_freebsd_misc.c \
	msm_freebsd_platform.c \
	msm_freebsd_power.c \
	msm_freebsd_soc.c

SRCS+=	opt_acpi.h \
	opt_drm.h \
	opt_platform.h \
	opt_vm.h \
	acpi_if.h \
	bus_if.h \
	device_if.h \
	iicbus_if.h \
	pci_if.h \
	pci_iov_if.h \
	vnode_if.h

.include <bsd.kmod.mk>

.include "${DRMKMOD}/linuxkpi_version.mk"
.include "${DRMKMOD}/compiler_flags.mk"

CFLAGS+= -I${.CURDIR:H}/freebsd/include
CFLAGS+= -I${.CURDIR:H}/include
CFLAGS+= -I${.CURDIR:H}/include/uapi
CFLAGS+= -I${MSMDIR}
CFLAGS+= -I${.CURDIR:H}/freebsd
CFLAGS+= -I${MSMDIR}/generated
CFLAGS+= -I${DRMKMOD}/linuxkpi/gplv2/include
CFLAGS+= -I${DRMKMOD}/linuxkpi/bsd/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/common/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/dummy/include
CFLAGS+= -I${DRMKMOD}/include
CFLAGS+= -I${DRMKMOD}/include/drm
CFLAGS+= -I${DRMKMOD}/include/uapi
CFLAGS+= -I${DRMKMOD}/include/uapi/drm
CFLAGS+= -I${DRMKMOD}/drivers/gpu

CFLAGS+= '-DKBUILD_MODNAME="${KMOD}"'
CFLAGS+= '-DLINUXKPI_PARAM_PREFIX=msm_' -DDRM_SYSCTL_PARAM_PREFIX=_${KMOD}
CFLAGS+= ${KCONFIG:C/(.*)/-DCONFIG_\1/}
CFLAGS+= -include ${.CURDIR:H}/freebsd/include/msm_freebsd_compat.h
CFLAGS+= -DCONFIG_ARCH_QCOM -DCONFIG_DRM_MSM -DCONFIG_QCOM_COMMAND_DB -DCONFIG_QCOM_MDT_LOADER

CFLAGS.msm_freebsd_bus.c+= -DMSM_FBSD_BUS

CWARNFLAGS+=	-Wno-format -Wno-gnu-pointer-arith -Wno-cast-qual
