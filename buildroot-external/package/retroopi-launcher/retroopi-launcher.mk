################################################################################
#
# retroopi-launcher
#
################################################################################

RETROOPI_LAUNCHER_VERSION = local
RETROOPI_LAUNCHER_SITE = $(BR2_EXTERNAL_RETROOPI_PATH)/../launcher
RETROOPI_LAUNCHER_SITE_METHOD = local
RETROOPI_LAUNCHER_LICENSE = GPL-2.0
RETROOPI_LAUNCHER_DEPENDENCIES = libdrm sdl2 sdl2_ttf sdl2_image libpng freetype zlib

RETROOPI_LAUNCHER_SCREEN_W = $(call qstrip,$(BR2_PACKAGE_RETROOPI_LAUNCHER_SCREEN_W))
RETROOPI_LAUNCHER_SCREEN_H = $(call qstrip,$(BR2_PACKAGE_RETROOPI_LAUNCHER_SCREEN_H))

define RETROOPI_LAUNCHER_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		STRIP="$(TARGET_STRIP)" \
		SYSROOT="$(STAGING_DIR)" \
		SCREEN_W=$(RETROOPI_LAUNCHER_SCREEN_W) \
		SCREEN_H=$(RETROOPI_LAUNCHER_SCREEN_H)
endef

define RETROOPI_LAUNCHER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/retroopi_launcher \
		$(TARGET_DIR)/usr/bin/retroopi_launcher
endef

$(eval $(generic-package))
