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
