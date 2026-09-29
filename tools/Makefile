# Test programs for msm and the Mesa drivers on it; see README.md.
# They need libdrm's headers, and egltest and vktest need Mesa and the
# Vulkan loader: they are not built with the kernel module.

LOCALBASE?=	/usr/local

PROGS=		msmtest msmfault egltest vktest
MAN=

CFLAGS+=	-I${.CURDIR}/../include/uapi/drm -I${LOCALBASE}/include/libdrm \
		-I${LOCALBASE}/include
LDFLAGS+=	-L${LOCALBASE}/lib
LDADD.egltest=	-lEGL -lGLESv2 -lgbm
LDADD.vktest=	-lvulkan

.include <bsd.progs.mk>
