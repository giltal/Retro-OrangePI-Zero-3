################################################################################
#
# BEETLESUPERGRAFX
#
################################################################################
LIBRETRO_BEETLESUPERGRAFX_VERSION = 0d4d96428073f8734e80a2ebc157daa228babe51
LIBRETRO_BEETLESUPERGRAFX_SITE = $(call github,libretro,beetle-supergrafx-libretro,$(LIBRETRO_BEETLESUPERGRAFX_VERSION))

# On AArch64 "buildroot gles armv8" lands in the Makefile's `findstring armv`
# branch, which unconditionally adds -marm (aarch64 gcc error). Use unix
# there instead. That branch sets IS_X86 from the HOST's `uname -p`, so force
# it off; it only adds an unused -DARCH_X86 today, but it is wrong.
ifeq ($(BR2_aarch64),y)
LIBRETRO_BEETLESUPERGRAFX_PLATFORM = unix
LIBRETRO_BEETLESUPERGRAFX_CONF = IS_X86=0
else
LIBRETRO_BEETLESUPERGRAFX_PLATFORM = $(LIBRETRO_PLATFORM)
endif

define LIBRETRO_BEETLESUPERGRAFX_BUILD_CMDS
	CFLAGS="$(TARGET_CFLAGS)" CXXFLAGS="$(TARGET_CXXFLAGS)" \
	       LDFLAGS="$(TARGET_LDFLAGS) -lstdc++ -lm" \
	       $(MAKE) -C $(@D) \
	       CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" LD="$(TARGET_CC)" \
	       RANLIB="$(TARGET_RANLIB)" AR="$(TARGET_AR)" \
	       platform="$(LIBRETRO_BEETLESUPERGRAFX_PLATFORM)" $(LIBRETRO_BEETLESUPERGRAFX_CONF)
endef

define LIBRETRO_BEETLESUPERGRAFX_INSTALL_TARGET_CMDS
	$(INSTALL) -D $(@D)/mednafen_supergrafx_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/beetlesupergrafx_libretro.so
endef

$(eval $(generic-package))
