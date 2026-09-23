#!/bin/bash
#
# Talk to the board over Ethernet + SSH. Run inside WSL:
#
#   wsl bash /mnt/c/OrangePI_Projects/RetroOPI_Z3/scripts/board.sh <cmd> [args]
#
#   status                 hostname, uptime, IP, what is running, temps, DRM/ALSA
#   push <path>...         copy target/ files (absolute board paths, e.g.
#                          /usr/bin/retroopi_launcher) from ~/opi/output/target,
#                          restarting the launcher around it; verifies md5
#   kernel                 install Image + DTB + extlinux.conf, keeping .prev copies
#   run <script> [args]    copy a local shell script over, strip CR, run it
#   ssh [cmd]              interactive shell, or one command
#
# Board address: $BOARD, else ~/opi/board_ip (write the IP there once).
#
# Conventions carried over from the reference (RetroBPI_M2M scripts/):
#  - never touch the board while a game is running: a launcher restart under
#    RetroArch loses the game, and pushing a core it has mapped crashes it
#  - print the board-side md5 next to the host-side md5. "scp succeeded" is not
#    the same as "the board is running the new file"
#  - host keys change on every reflash (dropbear regenerates them), so skip
#    known_hosts entirely
#  - dropbear has no sftp-server, so scp needs -O (legacy protocol)
set -eo pipefail

BOARD=${BOARD:-$(cat ~/opi/board_ip 2>/dev/null || true)}
[ -n "$BOARD" ] || { echo "board.sh: set BOARD=<ip> or write it to ~/opi/board_ip" >&2; exit 1; }

KEY=~/.ssh/retroopi_ed25519
O=(-o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
   -o ConnectTimeout=10 -o LogLevel=ERROR)
[ -f "$KEY" ] && O+=(-i "$KEY")

T=~/opi/output/target
I=~/opi/output/images

rsh()  { ssh "${O[@]}" "root@$BOARD" "$@"; }
rcp()  { scp -O "${O[@]}" "$@"; }

game_running() {
	[ "$(rsh 'pidof retroarch.bin >/dev/null && echo y || echo n')" = y ]
}

cmd=${1:-status}; shift || true
case "$cmd" in
status)
	rsh '
	echo "host    : $(hostname)  $(uname -r)"
	echo "uptime  : $(cut -d" " -f1 /proc/uptime) s"
	echo "ip      : $(ip -4 -o addr show eth0 2>/dev/null | awk "{print \$4}")"
	echo "launcher: $(pidof retroopi_launcher >/dev/null && echo running || echo NOT running)"
	echo "game    : $(pidof retroarch.bin >/dev/null && echo running || echo none)"
	echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null) @ $(( $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0) / 1000 )) MHz"
	for z in /sys/class/thermal/thermal_zone*; do
		[ -r $z/temp ] && echo "thermal : $(cat $z/type) $(( $(cat $z/temp) / 1000 )) C"
	done
	for c in /sys/class/drm/card*-*; do
		[ -r $c/status ] && echo "drm     : ${c##*/} $(cat $c/status) $(head -1 $c/modes 2>/dev/null)"
	done
	echo "alsa    :"; sed "s/^/          /" /proc/asound/cards 2>/dev/null
	echo "gpu     : $(ls /sys/class/devfreq 2>/dev/null | tr "\n" " ")"
	[ -f /run/boottiming ] && echo "boot    : rcS done at $(sed -n "s/^end //p" /run/boottiming) s"
	'
	;;
push)
	[ $# -gt 0 ] || { echo "usage: board.sh push /board/path..." >&2; exit 1; }
	if game_running; then echo "ABORT: a game is running on the board."; exit 2; fi
	for p in "$@"; do
		[ -f "$T$p" ] || { echo "not in target/: $p" >&2; exit 1; }
		rcp "$T$p" "root@$BOARD:/tmp/push.$$.$(basename "$p")"
	done
	rsh "/etc/init.d/S12launcher stop >/dev/null 2>&1 || true"
	for p in "$@"; do
		rsh "cp '/tmp/push.$$.$(basename "$p")' '$p' && rm -f '/tmp/push.$$.$(basename "$p")'"
	done
	rsh "sync; /etc/init.d/S12launcher start >/dev/null 2>&1"
	for p in "$@"; do
		b=$(rsh "md5sum '$p'" | cut -d' ' -f1)
		h=$(md5sum "$T$p" | cut -d' ' -f1)
		[ "$b" = "$h" ] && echo "OK       $p  $b" || { echo "MISMATCH $p  board $b host $h"; exit 1; }
	done
	;;
kernel)
	if game_running; then echo "ABORT: a game is running on the board."; exit 2; fi
	DTB=sun50i-h618-orangepi-zero3.dtb
	R=/mnt/c/OrangePI_Projects/RetroOPI_Z3/buildroot-external/board/opi-zero3/rootfs_overlay
	# Keep the last known-good set, so a bad kernel is recoverable from the
	# U-Boot prompt on the serial console instead of by reflashing the card.
	rsh "cd /boot && cp -n Image Image.prev && cp -n $DTB $DTB.prev && cp -n extlinux/extlinux.conf extlinux/extlinux.conf.prev; true"
	rcp "$I/Image" "root@$BOARD:/boot/Image.new"
	rcp "$I/$DTB" "root@$BOARD:/boot/$DTB.new"
	rcp "$R/boot/extlinux/extlinux.conf" "root@$BOARD:/boot/extlinux/extlinux.conf.new"
	rsh "set -e; cd /boot
		sed -i 's/\r\$//' extlinux/extlinux.conf.new
		grep -q '^ *kernel /boot/Image' extlinux/extlinux.conf.new
		grep -q 'root=/dev/mmcblk0p1' extlinux/extlinux.conf.new
		mv Image.new Image; mv $DTB.new $DTB; mv extlinux/extlinux.conf.new extlinux/extlinux.conf
		sync
		echo \"board Image: \$(md5sum Image | cut -d' ' -f1)\""
	echo "host  Image: $(md5sum "$I/Image" | cut -d' ' -f1)"
	echo "Reboot to use it. Fallback: Image.prev / $DTB.prev / extlinux.conf.prev"
	;;
run)
	[ -f "${1:-}" ] || { echo "usage: board.sh run <script> [args]" >&2; exit 1; }
	s=$1; shift
	rcp "$s" "root@$BOARD:/tmp/run.sh"
	rsh "sed -i 's/\r\$//' /tmp/run.sh && sh /tmp/run.sh $*"
	;;
ssh)
	if [ $# -eq 0 ]; then ssh "${O[@]/BatchMode=yes/BatchMode=no}" "root@$BOARD"; else rsh "$@"; fi
	;;
*)
	sed -n '3,15p' "$0"; exit 1
	;;
esac
