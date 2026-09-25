################################################################################
#
# PPSSPP -- PlayStation Portable
#
################################################################################
# Upstream PPSSPP, not the old libretro/ppsspp fork this package used to pin
# (a 2018 commit, with Rockchip patches for options that upstream has had for
# years). Upstream builds the libretro core itself with -DLIBRETRO=ON.
#
# git + submodules: PPSSPP vendors its dependencies (glslang, SPIRV-Cross,
# libchdr, zstd, rcheevos...) and a PREBUILT FFmpeg (ffmpeg/linux/aarch64),
# none of which are in a GitHub tarball.
LIBRETRO_PPSSPP_VERSION = v1.20.4
LIBRETRO_PPSSPP_SITE = https://github.com/hrydgard/ppsspp.git
LIBRETRO_PPSSPP_SITE_METHOD = git
LIBRETRO_PPSSPP_GIT_SUBMODULES = YES
LIBRETRO_PPSSPP_LICENSE = GPL-2.0+
LIBRETRO_PPSSPP_DEPENDENCIES = libpng zlib

LIBRETRO_PPSSPP_CONF_OPTS += -DLIBRETRO=ON
# Link the vendored sub-libraries INTO the core. Buildroot's cmake-package
# passes BUILD_SHARED_LIBS=ON by default, so cpu_features etc. were built as
# separate .so files that are never installed, and on the board the core
# failed to dlopen: "libcpu_features.so: cannot open shared object file"
# (found with tools/core-info). A libretro core must be self-contained.
LIBRETRO_PPSSPP_CONF_OPTS += -DBUILD_SHARED_LIBS=OFF
# GLES, not desktop GL. With USING_GLES2 the libretro port skips the GL core
# profile and takes RetroArch's GLES3 context, which is what panfrost on the
# Mali-G31 provides (GLES 3.1). The name is historical: it means "a GLES
# target", not "GLES 2.0 only". RetroArch must be built with
# --enable-opengles3 (see retroarch.mk).
LIBRETRO_PPSSPP_CONF_OPTS += -DUSING_GLES2=ON
LIBRETRO_PPSSPP_CONF_OPTS += -DUSING_EGL=OFF
# No window system here: KMS/GBM only. Vulkan code still builds (PPSSPP
# loads libvulkan at runtime), but no X11/Wayland WSI.
LIBRETRO_PPSSPP_CONF_OPTS += -DUSING_X11_VULKAN=OFF
LIBRETRO_PPSSPP_CONF_OPTS += -DUSE_WAYLAND_WSI=OFF
LIBRETRO_PPSSPP_CONF_OPTS += -DUSE_VULKAN_DISPLAY_KHR=OFF
# Bundled FFmpeg (for PMF video cutscenes), not the system's.
LIBRETRO_PPSSPP_CONF_OPTS += -DUSE_FFMPEG=ON
LIBRETRO_PPSSPP_CONF_OPTS += -DUSE_SYSTEM_FFMPEG=OFF
LIBRETRO_PPSSPP_CONF_OPTS += -DUSE_SYSTEM_LIBPNG=ON
LIBRETRO_PPSSPP_CONF_OPTS += -DUSE_DISCORD=OFF
LIBRETRO_PPSSPP_CONF_OPTS += -DUSE_MINIUPNPC=OFF
LIBRETRO_PPSSPP_CONF_OPTS += -DHEADLESS=OFF
LIBRETRO_PPSSPP_CONF_OPTS += -DUNITTEST=OFF
LIBRETRO_PPSSPP_CONF_OPTS += -DCMAKE_BUILD_TYPE=Release
# -O3, see libretro-flycast.mk: Buildroot's CMake toolchain file empties the
# Release flags, which left the -O2 from CMAKE_CXX_FLAGS in effect.
# (A few vendored libraries pin -O2 themselves in PPSSPP's CMakeLists.)
LIBRETRO_PPSSPP_CONF_OPTS += -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG"
LIBRETRO_PPSSPP_CONF_OPTS += -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG"

# The core looks for its assets (fonts, PPGe atlas, language files) in
# <system_directory>/PPSSPP. Our system_directory is the FAT partition
# (/opt/roms/_system/bios), which is created at image time and must never be
# overwritten on a card that already has ROMs. So the assets ship in the
# rootfs, and S46ppsspp copies them onto the card only if they are missing.
define LIBRETRO_PPSSPP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/lib/ppsspp_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/ppsspp_libretro.so
	rm -rf $(TARGET_DIR)/usr/share/ppsspp/PPSSPP
	mkdir -p $(TARGET_DIR)/usr/share/ppsspp/PPSSPP
	cp -r $(@D)/assets/. $(TARGET_DIR)/usr/share/ppsspp/PPSSPP/
	rm -rf $(TARGET_DIR)/usr/share/ppsspp/PPSSPP/debugger
	echo "$(LIBRETRO_PPSSPP_VERSION)" > $(TARGET_DIR)/usr/share/ppsspp/PPSSPP/.version
endef

$(eval $(cmake-package))
