################################################################################
#
# FUSE
#
################################################################################
LIBRETRO_FUSE_VERSION = 69a44421bd82b7ed4b389b1f997e6d5153ea855c
LIBRETRO_FUSE_SITE = $(call github,libretro,fuse-libretro,$(LIBRETRO_FUSE_VERSION))

# On AArch64 "buildroot gles armv8" lands in Makefile.libretro's `findstring
# armv` branch: -marm (aarch64 gcc error) and CC/CC_AS forced to the host
# `gcc`. The generic unix branch adds no CPU flags; TARGET_CFLAGS supplies
# -mcpu=cortex-a53.
ifeq ($(BR2_aarch64),y)
LIBRETRO_FUSE_PLATFORM = unix
else
LIBRETRO_FUSE_PLATFORM = $(LIBRETRO_PLATFORM)
endif

define LIBRETRO_FUSE_BUILD_CMDS
	CFLAGS="$(TARGET_CFLAGS) $(LIBRETRO_COMPAT_CFLAGS)" \
	       CXXFLAGS="$(TARGET_CXXFLAGS)" \
	       LDFLAGS="$(TARGET_LDFLAGS)" \
	       $(MAKE) -C $(@D) -f Makefile.libretro \
	       CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" LD="$(TARGET_CC)" \
	       RANLIB="$(TARGET_RANLIB)" AR="$(TARGET_AR)" \
	       platform="$(LIBRETRO_FUSE_PLATFORM)"
endef

define LIBRETRO_FUSE_INSTALL_TARGET_CMDS
	$(INSTALL) -D $(@D)/fuse_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/fuse_libretro.so
endef

$(eval $(generic-package))
