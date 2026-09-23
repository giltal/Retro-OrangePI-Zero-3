################################################################################
#
# ATARI800
#
################################################################################
# Added for this board: the ROM set on the card includes 92 Atari 800 titles
# (.atr disk images, .a52 5200 carts, .xex executables) and no core in the
# original tree handled them -- the launcher's system table already had an
# atari800 entry pointing at a core that did not exist, so the system simply
# never appeared.
LIBRETRO_ATARI800_VERSION = cd721790a0aa0e0772810949abcf5bd699c15371
LIBRETRO_ATARI800_SITE = $(call github,libretro,libretro-atari800,$(LIBRETRO_ATARI800_VERSION))

define LIBRETRO_ATARI800_BUILD_CMDS
	CFLAGS="$(TARGET_CFLAGS) $(LIBRETRO_COMPAT_CFLAGS)" \
	       CXXFLAGS="$(TARGET_CXXFLAGS)" \
	       LDFLAGS="$(TARGET_LDFLAGS)" \
	       $(MAKE) -C $(@D) \
	       CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" LD="$(TARGET_CC)" \
	       RANLIB="$(TARGET_RANLIB)" AR="$(TARGET_AR)" \
	       platform="unix"
endef

define LIBRETRO_ATARI800_INSTALL_TARGET_CMDS
	$(INSTALL) -D $(@D)/atari800_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/atari800_libretro.so
endef

$(eval $(generic-package))
