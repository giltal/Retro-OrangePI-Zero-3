################################################################################
#
# FBALPHA2012
#
################################################################################
LIBRETRO_FBALPHA2012_VERSION = c1de0e8c43d6560d9232e57ada6bbb8460168b5a
LIBRETRO_FBALPHA2012_SITE = $(call github,libretro,fbalpha2012,$(LIBRETRO_FBALPHA2012_VERSION))

LIBRETRO_FBALPHA2012_SUBDIR = svn-current/trunk

# EXTERNAL_ZLIB=1 links -lz, so zlib must be built first. Without the
# dependency this only worked when something else happened to build zlib
# earlier; a clean `make libretro-fbalpha2012` fails with "cannot find -lz".
ifeq ($(BR2_PACKAGE_ZLIB),y)
LIBRETRO_FBALPHA2012_CONF += EXTERNAL_ZLIB=1
LIBRETRO_FBALPHA2012_DEPENDENCIES += zlib
endif

ifeq ($(BR2_PACKAGE_LIBRETRO_FBALPHA2012_PGM_NEOGEO_ONLY),y)
	LIBRETRO_FBALPHA2012_CONF += NO_MD=1
	LIBRETRO_FBALPHA2012_CONF += NO_CPS=1
	LIBRETRO_FBALPHA2012_CONF += NO_PCE=1
	LIBRETRO_FBALPHA2012_CONF += CPS2_DIR=
	LIBRETRO_FBALPHA2012_CONF += CPS3_DIR=
	LIBRETRO_FBALPHA2012_CONF += GALAXIAN_DIR=
	LIBRETRO_FBALPHA2012_CONF += SNES_DIR=
	LIBRETRO_FBALPHA2012_CONF += target=pgm_neogeo
endif

define LIBRETRO_FBALPHA2012_BUILD_CMDS
	CFLAGS="$(TARGET_CFLAGS) $(LIBRETRO_COMPAT_CFLAGS)" \
	       CXXFLAGS="$(TARGET_CXXFLAGS)" \
	       LDFLAGS="$(TARGET_LDFLAGS) -lstdc++ -lm" \
	       $(MAKE) -C $(LIBRETRO_FBALPHA2012_SRCDIR) -f makefile.libretro \
	       CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" LD="$(TARGET_CC)" \
	       RANLIB="$(TARGET_RANLIB)" AR="$(TARGET_AR)" \
	       platform="$(LIBRETRO_PLATFORM)" $(LIBRETRO_FBALPHA2012_CONF)
endef

define LIBRETRO_FBALPHA2012_INSTALL_TARGET_CMDS
	$(INSTALL) -D $(LIBRETRO_FBALPHA2012_SRCDIR)/fbalpha2012_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/fbalpha2012_libretro.so
endef

$(eval $(generic-package))
