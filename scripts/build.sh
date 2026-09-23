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

echo "build.sh: $(date '+%F %T') make ${*:-all} (log: $LOG)"
start=$(date +%s)
if make O="$OUT" -j"$(nproc)" "$@" > "$LOG" 2>&1; then
	echo "build.sh: OK after $(( ($(date +%s) - start) / 60 )) min"
else
	echo "build.sh: FAILED after $(( ($(date +%s) - start) / 60 )) min -- tail of $LOG:"
	tail -40 "$LOG"
	exit 1
fi

if [ $# -eq 0 ]; then
	mkdir -p "$PROJ/firmware"
	cp "$OUT/images/sdcard.img" "$PROJ/firmware/sdcard.img"
	md5sum "$OUT/images/sdcard.img" | tee "$PROJ/firmware/sdcard.img.md5"
	ls -la "$OUT/images/"
fi
