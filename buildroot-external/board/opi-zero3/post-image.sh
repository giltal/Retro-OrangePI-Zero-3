#!/bin/bash
#
# Build the FAT32 ROM partition image, then hand off to Buildroot's genimage.sh.
#
# The SD card gets two visible partitions:
#   p1  ext4   rootfs (kernel, DTBs and extlinux live in /boot inside it)
#   p2  FAT32  ROMs, mounted at /opt/roms -- readable/writable from Windows,
#              so ROMs can be dropped straight onto the card
#
# The launcher hardcodes /opt/roms with one subdirectory per system, plus
# _system for favorites/recents/state/theme (see launcher/launcher.c). Those
# directories are pre-created here so the partition is usable the moment the
# card is flashed, without having to boot the board first.
set -e

BINARIES_DIR="$1"
shift

ROMS_IMG="${BINARIES_DIR}/roms.vfat"
# Default sized for a 32 GB card. "32 GB" cards are sold in decimal GB, and
# their real capacity varies by brand from ~30.5e9 to ~32.0e9 bytes. The whole
# image is 1 MiB (bootloader) + 1024 MiB (rootfs) + this, so 27648 MiB makes
# a 28673 MiB (30.07e9 byte) image, which fits every 32 GB card seen so far.
# Override for other cards, e.g. RETROOPI_ROMS_SIZE_MB=2048 for a small
# dev image, or ~57000 for a 64 GB card.
ROMS_SIZE_MB="${RETROOPI_ROMS_SIZE_MB:-27648}"
LABEL="RETROROMS"

MKFS_VFAT="${HOST_DIR}/sbin/mkfs.vfat"
[ -x "$MKFS_VFAT" ] || MKFS_VFAT="${HOST_DIR}/bin/mkfs.vfat"
[ -x "$MKFS_VFAT" ] || MKFS_VFAT="$(command -v mkfs.vfat || true)"
MMD="${HOST_DIR}/bin/mmd"
MCOPY="${HOST_DIR}/bin/mcopy"

if [ ! -x "$MKFS_VFAT" ]; then
	echo "post-image.sh: ERROR: mkfs.vfat not found (need BR2_PACKAGE_HOST_DOSFSTOOLS)" >&2
	exit 1
fi

echo "post-image.sh: creating ${ROMS_SIZE_MB} MiB FAT32 ROM partition"
rm -f "$ROMS_IMG"
# Sparse, not dd from /dev/zero: at 27 GiB that is 27 GiB of zeros written to
# the build disk every image build. mkfs.vfat only writes the boot sector and
# FATs, and everything else stays a hole, which genimage and xz both skip cheaply.
truncate -s "${ROMS_SIZE_MB}M" "$ROMS_IMG"
"$MKFS_VFAT" -F 32 -n "$LABEL" "$ROMS_IMG" >/dev/null

# One directory per system the launcher knows about, so the folder names on the
# card match what it scans for. Keep this list in step with the systems table in
# launcher/launcher.c.
# Only systems we actually ship a core for. The launcher hides a folder whose
# core is missing, so an extra folder here is not broken -- it is just a place
# a user drops ROMs that then never appear, which is worse than no folder.
# (The reference left atari800 out of this list after its core had been added.)
SYSTEMS="nes snes gb gbc gba genesis mastersystem gamegear atari2600 atari7800 \
atari800 pce pcesupergrafx zxspectrum doom neogeo cps1 cps2 cps3 arcade mame \
n64 psx _system"

export MTOOLS_SKIP_CHECK=1
for d in $SYSTEMS; do
	"$MMD" -i "$ROMS_IMG" "::/$d"
done
"$MMD" -i "$ROMS_IMG" "::/_system/states"
"$MMD" -i "$ROMS_IMG" "::/_system/bios"

# A note for whoever opens the card on a PC.
TMPTXT="$(mktemp)"
cat > "$TMPTXT" <<'README_EOF'
RetroOPI_Z3 - ROM partition

Drop ROMs into the folder matching their system, e.g.:

    nes\Super Mario Bros.nes
    gb\Tetris.gb
    genesis\Sonic.md

The launcher scans these folders on startup and hides any that are empty.
_system holds favourites, recents, saved state and theme - leave it alone.
_system\bios is where BIOS files go (PlayStation: SCPH1001.BIN).
_system\audio_card (optional): a single digit forcing the ALSA sound card.

This partition is mounted at /opt/roms on the device.
README_EOF
"$MCOPY" -i "$ROMS_IMG" -o "$TMPTXT" "::/README.txt"
rm -f "$TMPTXT"

echo "post-image.sh: ROM partition ready: $(du -h "$ROMS_IMG" | cut -f1)"

# genimage.sh runs after this script: BR2_ROOTFS_POST_IMAGE_SCRIPT lists both,
# in order, and Buildroot passes the same arguments to each. We ignore the
# "-c <genimage.cfg>" args meant for genimage.
exit 0
