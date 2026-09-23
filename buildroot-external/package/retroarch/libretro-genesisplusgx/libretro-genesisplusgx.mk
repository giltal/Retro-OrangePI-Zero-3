################################################################################
#
# GENESISPLUSGX
#
################################################################################
LIBRETRO_GENESISPLUSGX_VERSION = 7856b7208981fa4dc33623f89c4f330a95019c26
LIBRETRO_GENESISPLUSGX_SITE = $(call github,libretro,Genesis-Plus-GX,$(LIBRETRO_GENESISPLUSGX_VERSION))

# On AArch64, BR2_ARM_CPU_ARMV8A is set for cortex_a53 as well, so the block
# below would append rpi3 -- the 32-bit Pi 3 flags (-DARM -marm
# -mfpu=neon-fp-armv8 -mfloat-abi=hard), which aarch64 gcc rejects. The
# generic unix branch has no CPU flags; TARGET_CFLAGS supplies -mcpu.
ifeq ($(BR2_aarch64),y)
LIBRETRO_GENESISPLUSGX_PLATFORM = unix
else
LIBRETRO_GENESISPLUSGX_PLATFORM = $(LIBRETRO_PLATFORM)

# Reusing RPI configs
ifeq ($(BR2_arm),y)
	LIBRETRO_GENESISPLUSGX_PLATFORM += rpi
endif
ifeq ($(BR2_ARM_CPU_ARMV6),y)
	LIBRETRO_GENESISPLUSGX_PLATFORM += rpi1
else ifeq ($(BR2_ARM_CPU_ARMV7A),y)
	LIBRETRO_GENESISPLUSGX_PLATFORM += rpi2
else ifeq ($(BR2_ARM_CPU_ARMV8A),y)
	LIBRETRO_GENESISPLUSGX_PLATFORM += rpi3
endif
endif

define LIBRETRO_GENESISPLUSGX_BUILD_CMDS
	CFLAGS="$(TARGET_CFLAGS) $(LIBRETRO_COMPAT_CFLAGS)" \
	       CXXFLAGS="$(TARGET_CXXFLAGS)" \
	       LDFLAGS="$(TARGET_LDFLAGS)" \
	       $(MAKE) -C $(@D)/ -f Makefile.libretro \
	       CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" LD="$(TARGET_CC)" \
	       RANLIB="$(TARGET_RANLIB)" AR="$(TARGET_AR)" \
	       platform="$(LIBRETRO_GENESISPLUSGX_PLATFORM)"
endef

define LIBRETRO_GENESISPLUSGX_INSTALL_TARGET_CMDS
	$(INSTALL) -D $(@D)/genesis_plus_gx_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/genesisplusgx_libretro.so
endef

$(eval $(generic-package))
