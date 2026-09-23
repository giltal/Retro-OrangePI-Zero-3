# Aggregator: pull in retroarch and every libretro core package.

# --- Compatibility flags for the older cores in this tree -----------------
#
# These packages were written against roughly GCC 8; this toolchain is GCC
# 14.3. Several C defaults changed underneath them, and the failures are all
# the language getting stricter rather than bugs in the emulation:
#
#   -fno-common became the default in GCC 10. The cores rely on tentative
#   definitions, so the same global lands in several objects and the link dies
#   with "multiple definition of 'config'" (genesisplusgx, fuse).
#
#   GCC 14 promoted a family of long-standing warnings to hard errors:
#   incompatible-pointer-types (fbalpha2012), implicit-function-declaration,
#   implicit-int, int-conversion and return-mismatch.
#
# These are applied PER CORE rather than added to global TARGET_CFLAGS. Making
# them global would silently weaken diagnostics for every other package on the
# system -- including ones where an implicit declaration really would be a bug
# worth failing on. A core opts in by using this variable, so it is visible in
# that core's .mk which compatibility it needs.
LIBRETRO_COMPAT_CFLAGS = \
	-fcommon \
	-Wno-incompatible-pointer-types \
	-Wno-implicit-function-declaration \
	-Wno-implicit-int \
	-Wno-int-conversion \
	-Wno-return-mismatch

include $(sort $(wildcard $(BR2_EXTERNAL_RETROOPI_PATH)/package/retroarch/*/*.mk))
