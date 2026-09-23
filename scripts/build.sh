#!/bin/bash
#
# Build the RetroOPI_Z3 image in WSL.
#
#   wsl bash /mnt/c/OrangePI_Projects/RetroOPI_Z3/scripts/build.sh            # full build
#   wsl bash /mnt/c/OrangePI_Projects/RetroOPI_Z3/scripts/build.sh linux      # one target
#   wsl bash /mnt/c/OrangePI_Projects/RetroOPI_Z3/scripts/build.sh defconfig  # re-apply defconfig
#
# Layout (kept apart from the reference project's ~/bpi tree):
#   ~/opi/buildroot   Buildroot 2026.02.3, cloned from ~/bpi/buildroot
#   ~/opi/output      out-of-tree build (O=)
#   ~/bpi/dl          download cache, SHARED with RetroBPI_M2M on purpose --
#                     Buildroot keys downloads by package and version, so sharing
#                     is safe and saves re-fetching ~800 MB
#
# Output: ~/opi/output/images/sdcard.img. It is copied to firmware/ in the
# project, which is gitignored.
set -eo pipefail

PROJ=/mnt/c/OrangePI_Projects/RetroOPI_Z3
BR=~/opi/buildroot
OUT=~/opi/output
export BR2_DL_DIR=~/bpi/dl
LOG=~/opi/build.log

if [ ! -d "$BR" ]; then
	echo "build.sh: cloning Buildroot 2026.02.3 into $BR"
	git clone -q --branch 2026.02.3 ~/bpi/buildroot "$BR" 2>/dev/null \
		|| git clone -q --depth 1 --branch 2026.02.3 https://gitlab.com/buildroot.org/buildroot.git "$BR"
fi
cd "$BR"

if [ ! -f "$OUT/.config" ] || [ "${1:-}" = "defconfig" ]; then
	make BR2_EXTERNAL="$PROJ/buildroot-external" O="$OUT" opi_zero3_retro_defconfig
	[ "${1:-}" = "defconfig" ] && exit 0
fi

# Parallelism: BR2_JLEVEL jobs INSIDE each package, one package at a time.
#
# Build #1 used a top-level `make -j$(nproc)` instead, which lets Buildroot
# build several packages concurrently, each with its own nproc jobs. On clang
# that meant dozens of cc1plus processes at 0.5-2 GB each, and the OOM killer
# took one mid-build (WSL has 15 GB). Lower JLEVEL further if it recurs:
#   JLEVEL=8 bash scripts/build.sh clang
JLEVEL=${JLEVEL:-$(nproc)}

echo "build.sh: $(date '+%F %T') make ${*:-all} BR2_JLEVEL=$JLEVEL (log: $LOG)"
start=$(date +%s)
if make O="$OUT" BR2_JLEVEL="$JLEVEL" "$@" > "$LOG" 2>&1; then
	echo "build.sh: OK after $(( ($(date +%s) - start) / 60 )) min"
else
	echo "build.sh: FAILED after $(( ($(date +%s) - start) / 60 )) min -- tail of $LOG:"
	tail -40 "$LOG"
	exit 1
fi

if [ $# -eq 0 ]; then
	F="$PROJ/firmware"
	I="$OUT/images/sdcard.img"
	mkdir -p "$F"
	# IMAGE_TAG names the deliverable, so image variants can sit side by side:
	#   bash build.sh                                          -> sdcard.img.xz (32 GB card)
	#   RETROOPI_ROMS_SIZE_MB=2048 IMAGE_TAG=small bash build.sh -> sdcard-small.img(.xz)
	# The small one flashes in about a minute; its FAT partition is last on the
	# card, so it can be grown afterwards with a partition tool.
	N="sdcard${IMAGE_TAG:+-$IMAGE_TAG}"
	# The image is sized for a 32 GB card (see ROMS_SIZE_MB in post-image.sh)
	# and is almost entirely empty FAT space, sparse on the WSL side. Copying it
	# raw to /mnt/c would write every hole out as real zeros, so the
	# deliverable is xz-compressed: Etcher and Rufus both flash .xz directly.
	# The raw .img is only copied when it is small enough to be convenient.
	rm -f "$F/$N.img" "$F/$N.img.md5"
	echo "build.sh: compressing $N.img ($(du -h --apparent-size "$I" | cut -f1) apparent, $(du -h "$I" | cut -f1) on disk)"
	xz -T0 -3 -c "$I" > "$F/$N.img.xz.tmp" && mv "$F/$N.img.xz.tmp" "$F/$N.img.xz"
	if [ "$(stat -c %s "$I")" -le $((4 * 1024 * 1024 * 1024)) ]; then
		cp --sparse=always "$I" "$F/$N.img"
	fi
	md5sum < "$I" | sed "s|-\$|$N.img|" | tee "$F/$N.img.md5"
	ls -la "$F"
fi
