################################################################################
#
# FLYCAST -- Sega Dreamcast / NAOMI / Atomiswave
#
################################################################################
# New for the Zero3: the Mali-G31 has GLES3, which Flycast's renderer needs
# (the reference's Mali-400 is GLES2 only). The ARM64 dynarec (vixl) is picked
# automatically from the architecture.
#
# git + submodules: libchdr, glslang, xbyak/vixl deps, rcheevos etc. are
# submodules and are not in a GitHub tarball.
LIBRETRO_FLYCAST_VERSION = v2.7
LIBRETRO_FLYCAST_SITE = https://github.com/flyinghead/flycast.git
LIBRETRO_FLYCAST_SITE_METHOD = git
LIBRETRO_FLYCAST_GIT_SUBMODULES = YES
LIBRETRO_FLYCAST_LICENSE = GPL-2.0+
LIBRETRO_FLYCAST_DEPENDENCIES = zlib libgles

LIBRETRO_FLYCAST_CONF_OPTS += -DLIBRETRO=ON
# Link the vendored sub-libraries INTO the core. Buildroot's cmake-package
# passes BUILD_SHARED_LIBS=ON by default, so nowide etc. were built as
# separate .so files that are never installed, and on the board the core
# failed to dlopen: "nowide.so.11.3.0: cannot open shared object file"
# (found with tools/core-info). A libretro core must be self-contained.
LIBRETRO_FLYCAST_CONF_OPTS += -DBUILD_SHARED_LIBS=OFF
# GLES3 (defines GLES GLES3 HAVE_OPENGLES3). RetroArch must be built with
# --enable-opengles3, see retroarch.mk.
LIBRETRO_FLYCAST_CONF_OPTS += -DUSE_GLES=ON
LIBRETRO_FLYCAST_CONF_OPTS += -DUSE_VULKAN=OFF
# Deterministic: our toolchain has no libgomp, and a find_package() that
# silently flips behaviour between build hosts is not wanted.
LIBRETRO_FLYCAST_CONF_OPTS += -DUSE_OPENMP=OFF
LIBRETRO_FLYCAST_CONF_OPTS += -DUSE_BREAKPAD=OFF
LIBRETRO_FLYCAST_CONF_OPTS += -DUSE_LUA=OFF
LIBRETRO_FLYCAST_CONF_OPTS += -DUSE_DISCORD=OFF
LIBRETRO_FLYCAST_CONF_OPTS += -DUSE_HOST_LIBZIP=OFF
LIBRETRO_FLYCAST_CONF_OPTS += -DCMAKE_BUILD_TYPE=Release

define LIBRETRO_FLYCAST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/flycast_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/flycast_libretro.so
endef

$(eval $(cmake-package))
