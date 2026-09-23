################################################################################
#
# PCSX
#
################################################################################
# The commit this package originally pinned (c88070d1) 404s -- upstream
# rewrote history, so the tarball no longer exists at codeload. Repinned to
# a current master head (2026-08-01), resolved from the GitHub API rather
# than guessed.
LIBRETRO_PCSX_VERSION = da2cb8ecd17fd0932ab6d94774c0522beebce6e3
LIBRETRO_PCSX_SITE = $(call github,libretro,pcsx_rearmed,$(LIBRETRO_PCSX_VERSION))

# platform=unix, NOT $(LIBRETRO_PLATFORM).
#
# Makefile.libretro branches on EXACT platform names (unix, linux-portable,
# osx, vita, ctr...). The composite string this tree builds --
# "buildroot gles armv7 hardfloat neon" -- matches none of them, so being
# explicit is the honest thing to do. ARM detection is independent either way:
# the build banner reports "armv7 neon ari64 gpu=neon" with both.
# On AArch64 the same holds: ARCH comes from `$(CC) -dumpmachine` (the target
# toolchain, not the host), giving ARCH=aarch64 -> DYNAREC=ari64 with
# new_dynarec/linkage_arm64.o, gte_arm64.o and BUILTIN_GPU=neon (the C/NEON
# intrinsics renderer; the 32-bit NEON asm is correctly skipped).
#
# NOTE: this was NOT the cause of the core failing to dlopen. That was an
# upstream OBJS omission, fixed by patch 0001 -- see there. Changing platform
# alone left the same two undefined symbols.

define LIBRETRO_PCSX_BUILD_CMDS
	CFLAGS="$(TARGET_CFLAGS)" CXXFLAGS="$(TARGET_CXXFLAGS)" \
	       LDFLAGS="$(TARGET_LDFLAGS)" \
	       $(MAKE) -C $(@D)/ -f Makefile.libretro \
	       CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" LD="$(TARGET_CC)" \
	       RANLIB="$(TARGET_RANLIB)" AR="$(TARGET_AR)" \
	       platform="unix"
endef

define LIBRETRO_PCSX_INSTALL_TARGET_CMDS
	$(INSTALL) -D $(@D)/pcsx_rearmed_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/pcsx_rearmed_libretro.so
endef

$(eval $(generic-package))
