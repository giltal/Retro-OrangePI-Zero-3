#!/bin/sh
#
# post-build: check invariants on target/ before it becomes an image.
#
# Buildroot's target/ directory is INCREMENTAL. Disabling a package or renaming
# an overlay file does not delete what a previous build already installed, so
# a stale copy silently survives into the image. The reference project
# (RetroBPI_M2M) shipped four wrong images that way before these guards existed:
# a renamed init script that started the launcher twice, a deleted config override
# that kept adding 128 ms of audio lag, and an instrumented core built from
# source that had already been cleaned.
#
# When you rename or remove an overlay file, add an `rm -f` for the old path
# here. A `make clean` also fixes it, but it rebuilds RetroArch and every core.
#
# TARGET_DIR is passed as $1 by Buildroot.

set -e
TARGET_DIR="$1"

fail() {
	echo "post-build.sh: ERROR: $*" >&2
	exit 1
}

# --- Known-stale paths ----------------------------------------------------
# Renamed 2026-09-23: S35alsa -> S11alsa (must run before the launcher).
rm -f "$TARGET_DIR/etc/init.d/S35alsa"
# Renamed 2026-09-24 before it ever shipped: S46ppsspp -> S46card.
rm -f "$TARGET_DIR/etc/init.d/S46ppsspp"

# --- Bring-up: a login prompt on the HDMI console -----------------------
# Buildroot's inittab only runs a getty on the serial port. If the launcher
# fails to start on a board with no serial adapter attached, the TV should
# at least show a login prompt, which proves the kernel, fbcon and userspace are
# all up. The launcher takes DRM master while it runs, so this is invisible
# in normal use. Appended here because the overlay would have to replace the
# whole generated inittab.
# (busybox inittab has no inline comments -- anything after the command is
# passed to it as arguments -- so the comment goes on its own line.)
sed -i '/^tty1::/d; /^# RetroOPI: HDMI console/d' "$TARGET_DIR/etc/inittab"
printf '# RetroOPI: HDMI console (added by post-build.sh)\ntty1::respawn:/sbin/getty 38400 tty1\n' \
	>> "$TARGET_DIR/etc/inittab"

# --- Dev convenience: this build host's SSH key ---------------------------
# Every reflash wiped /root/.ssh/authorized_keys, so board.sh stopped working
# until the key was installed again by hand with the root password. If the
# BUILD HOST has the dev key, bake its PUBLIC half into this image. Nothing
# enters the repository: an image built anywhere else simply gets no key.
devkey="$HOME/.ssh/retroopi_ed25519.pub"
if [ -f "$devkey" ]; then
	install -d -m 700 "$TARGET_DIR/root/.ssh"
	install -m 600 "$devkey" "$TARGET_DIR/root/.ssh/authorized_keys"
	echo "post-build.sh: dev SSH key from $devkey installed for root"
fi

# --- Arcade game titles for the launcher ----------------------------------
# Arcade ROMs are named after the emulator's driver ("mslug.zip"). The full
# titles are extracted from the source of the exact FBA 2012 / MAME 2003-Plus
# versions being built, so the launcher can show "Metal Slug - Super
# Vehicle-001" and hide BIOS sets (neogeo.zip). See scripts/gen-arcade-names.py.
names_dir="$TARGET_DIR/usr/share/retroopi/names"
fba_src=$(ls -d "$BUILD_DIR"/libretro-fbalpha2012-*/ 2>/dev/null | head -n 1)
mame_src=$(ls -d "$BUILD_DIR"/libretro-mame2003plus-*/ 2>/dev/null | head -n 1)
[ -n "$fba_src" ] || fail "libretro-fbalpha2012 build dir not found in $BUILD_DIR"
[ -n "$mame_src" ] || fail "libretro-mame2003plus build dir not found in $BUILD_DIR"
python3 "$BR2_EXTERNAL_RETROOPI_PATH/../scripts/gen-arcade-names.py" \
	"$fba_src" "$mame_src" "$names_dir"
for t in fbalpha2012 mame2003plus; do
	n=$(wc -l < "$names_dir/$t.txt")
	[ "$n" -gt 1000 ] || fail "$names_dir/$t.txt has only $n titles"
done
grep -qx 'neogeo=\*Neo Geo' "$names_dir/fbalpha2012.txt" \
	|| fail "neogeo is not marked as a BIOS set in fbalpha2012.txt"

# --- Guards ---------------------------------------------------------------
# Exactly one launcher init script.
n=$(ls "$TARGET_DIR"/etc/init.d/S??launcher 2>/dev/null | wc -l)
if [ "$n" -ne 1 ]; then
	ls "$TARGET_DIR"/etc/init.d/ >&2
	fail "expected 1 launcher init script, found $n"
fi

# The audio setup must run BEFORE the launcher, which sets the softvol Master
# control at startup. Checked by name so a renumbering cannot quietly break it.
alsa=$(ls "$TARGET_DIR"/etc/init.d/S??alsa 2>/dev/null | sed 's|.*/S\([0-9][0-9]\).*|\1|')
launch=$(ls "$TARGET_DIR"/etc/init.d/S??launcher | sed 's|.*/S\([0-9][0-9]\).*|\1|')
[ -n "$alsa" ] || fail "no S??alsa init script"
[ "$alsa" -lt "$launch" ] || fail "S${alsa}alsa must sort before S${launch}launcher"

# RetroArch audio: the threaded ALSA driver, through the softvol "default" PCM.
# Both are easy to lose to a stale target/ copy of retroarch.cfg.
racfg="$TARGET_DIR/root/.config/retroarch/retroarch.cfg"
grep -q '^audio_driver = "alsathread"' "$racfg" \
	|| fail "audio_driver is not alsathread in the shipped retroarch.cfg"
grep -q '^audio_device = "default"' "$racfg" \
	|| fail "audio_device is not \"default\" -- volume would not reach games"

# The softvol control S11alsa creates must be the one the launcher and volumed
# drive. softvol takes the name verbatim; the first image created 'Master'
# while both programs asked for 'Master Playback Volume', so volume silently
# did nothing while sound played fine.
grep -q 'name "Master Playback Volume"' "$TARGET_DIR/etc/init.d/S11alsa" \
	|| fail "S11alsa does not create the 'Master Playback Volume' softvol control"
for b in usr/bin/retroopi_launcher usr/bin/volumed; do
	strings "$TARGET_DIR/$b" | grep -q "'Master Playback Volume'" \
		|| fail "$b does not use the 'Master Playback Volume' control"
done

# The launcher and RetroArch must ask the TV for the SAME mode. If they differ,
# every game launch and exit is a full HDMI re-sync (seconds of black), and a
# mismatch is easy to create: one value is in the defconfig, the other in
# retroarch.cfg.
sw=$(sed -n 's/^BR2_PACKAGE_RETROOPI_LAUNCHER_SCREEN_W=//p' "$BR2_CONFIG")
sh=$(sed -n 's/^BR2_PACKAGE_RETROOPI_LAUNCHER_SCREEN_H=//p' "$BR2_CONFIG")
rx=$(sed -n 's/^video_fullscreen_x = "\([0-9]*\)"/\1/p' "$racfg")
ry=$(sed -n 's/^video_fullscreen_y = "\([0-9]*\)"/\1/p' "$racfg")
[ "$sw" = "$rx" ] && [ "$sh" = "$ry" ] \
	|| fail "launcher ${sw}x${sh} but retroarch.cfg video_fullscreen ${rx}x${ry}"

# Every system folder the card is given must be a system the launcher knows,
# checked in the BINARY: a local package can silently ship a stale build (see
# build.sh). The list comes from post-image.sh, so the two cannot drift.
systems=$(sed -n '/^SYSTEMS="/,/"$/p' "$BR2_EXTERNAL_RETROOPI_PATH/board/opi-zero3/post-image.sh" \
	| tr -d '"\\' | sed 's/^SYSTEMS=//' | tr ' ' '\n' | grep -v -E '^(_system)?$')
# -n 2: folder names like gb, nes, psp are shorter than strings' default
# minimum of 4 characters, and would never be printed.
lstr=$(strings -n 2 "$TARGET_DIR/usr/bin/retroopi_launcher")
for s in $systems; do
	printf '%s\n' "$lstr" | grep -qx "$s" \
		|| fail "launcher binary has no '$s' system entry -- stale build, or post-image.sh and launcher.c disagree"
done

# The launcher's symbol fallback font, at the path it hard-codes
# (SYMBOL_FONT_PATH). Without it, every UI symbol is a "missing glyph" box.
[ -f "$TARGET_DIR/usr/share/fonts/dejavu/DejaVuSans.ttf" ] \
	|| fail "no /usr/share/fonts/dejavu/DejaVuSans.ttf -- launcher symbols will render as boxes"

# Enough CMA for the scanout buffers at this resolution. At 1080p the kernel's
# default 32 MiB pool ran out and every game launch failed with
# "DRM_IOCTL_MODE_CREATE_DUMB failed: Cannot allocate memory".
grep -q ' cma=[0-9]*M' "$TARGET_DIR/boot/extlinux/extlinux.conf" \
	|| fail "extlinux.conf has no cma= -- 1080p scanout buffers will not fit in the default 32 MiB"

# Kernel and DTB where extlinux.conf says they are. A wrong fdt path boots to
# nothing at all, with the only clue on the serial console.
ext="$TARGET_DIR/boot/extlinux/extlinux.conf"
for f in $(sed -n 's/^[[:space:]]*\(kernel\|fdt\)[[:space:]]\+//p' "$ext"); do
	[ -e "$TARGET_DIR$f" ] || fail "extlinux.conf references $f, which is not in target/"
done

# Every line of the kernel config fragment must survive into the kernel's
# .config. Kconfig overrides a request it cannot satisfy WITHOUT any error:
# build #1 asked for SND=y and got =m throughout, because arm64 defconfig has
# SOUND=m, and BT=y came out =m because of RFKILL=m.
frag="$BR2_EXTERNAL_RETROOPI_PATH/board/opi-zero3/linux-retrogaming.config"
kcfg=$(ls -d "$BUILD_DIR"/linux-[0-9]*/.config 2>/dev/null | head -1)
if [ -f "$frag" ] && [ -f "$kcfg" ]; then
	bad=0
	for l in $(grep -E '^CONFIG_[A-Z0-9_]+=' "$frag"); do
		grep -qxF "$l" "$kcfg" || {
			echo "  wanted $l, got: $(grep "^${l%%=*}=" "$kcfg" || echo '<unset>')" >&2
			bad=1
		}
	done
	[ "$bad" -eq 0 ] || fail "kernel config fragment not honoured (see above)"
fi

# Every core's shared-library dependencies must exist in the target. A core
# that links against a library that was never installed builds fine and then
# fails to dlopen on the board, where RetroArch just returns to the launcher.
# That happened with PPSSPP (libcpu_features.so) and Flycast (nowide.so) under
# Buildroot's default BUILD_SHARED_LIBS=ON.
for so in "$TARGET_DIR"/usr/lib/libretro/*.so; do
	[ -e "$so" ] || continue
	for lib in $(readelf -d "$so" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
		[ -e "$TARGET_DIR/usr/lib/$lib" ] || [ -e "$TARGET_DIR/lib/$lib" ] \
			|| fail "$(basename "$so") needs $lib, which is not installed in the target"
	done
done

# No diagnostic instrumentation may reach an image. Restoring a core's SOURCE
# does not rebuild the binary, and target/ is incremental, so an instrumented
# .so can ship while the source looks clean. Verify the artifact.
for so in "$TARGET_DIR"/usr/lib/libretro/*.so; do
	[ -e "$so" ] || continue
	if strings "$so" 2>/dev/null | grep -qE 'astick\.log|glitch\.log|ra_audio\.log|speed_permille|RETROBPI_AUDIO_PROBE'; then
		fail "$(basename "$so") contains diagnostic instrumentation -- rebuild it from pristine source"
	fi
done

echo "post-build.sh: target/ OK; launcher: $(ls "$TARGET_DIR"/etc/init.d/S??launcher | sed 's|.*/||'), audio: S${alsa}alsa"
