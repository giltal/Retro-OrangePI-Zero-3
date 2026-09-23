# Flashing and first boot

## Write the card

`firmware/sdcard.img.xz` is a complete, compressed card image: the
SPL/TF-A/U-Boot blob at 8 KB, p1 = 1 GiB ext4 rootfs, p2 = 27 GiB FAT32 ROMs.
It is sized for a **32 GB card**: the uncompressed image is 30.07e9 bytes, and
32 GB cards hold roughly 30.5e9 to 32.0e9. For other card sizes, rebuild with
`RETROOPI_ROMS_SIZE_MB=<MiB>` (see `post-image.sh`).

- **Windows:** balenaEtcher or Rufus (DD mode). Both take the `.xz` directly.
  Pick the right disk. Writing takes a while: the card gets all ~30 GB.

**Faster alternative: `firmware/sdcard-small.img`** (2 GiB ROM partition,
built with `RETROOPI_ROMS_SIZE_MB=2048 IMAGE_TAG=small`). It flashes in about a
minute. Then grow `RETROROMS` to fill the card from Windows. **Disk Management
cannot extend FAT32**, so use a partition tool such as MiniTool Partition Wizard
or AOMEI Partition Assistant. The ROM partition is the last one on the card, so
it can grow in place, and the board mounts it the same way at any size.
- **Linux:** `xz -dc sdcard.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress`

A reflash **wipes the ROM partition**. For day-to-day iteration, push files
over SSH (`scripts/board.sh push`) instead of reflashing.

## Put ROMs on it

After flashing, Windows sees the `RETROROMS` FAT32 partition. Drop ROMs into
the per-system folders; see `README.txt` on the partition. The PSX BIOS goes in
`_system\bios\`.

## First boot checklist

Connect serial (3-pin debug header, 115200 8N1) **and** HDMI **and** Ethernet.

1. U-Boot banner, then the kernel on serial. The HDMI console should show kernel text via fbcon.
2. The launcher appears on HDMI at 1280×720. If it doesn't, check `/var/log/retroopi_launcher.log`.
   The launcher lists the modes the TV offers if 720p is missing.
3. Find the board's IP (router, or `ip addr` on serial). Then:
   `echo <ip> > ~/opi/board_ip` in WSL, and run `scripts/board.sh status`.
4. `board.sh status` should show: a DRM HDMI connector `connected`, an ALSA card
   with "hdmi" in its name, `performance` governor, thermal zones, and a devfreq entry for the GPU.
5. Start a game. Check `/tmp/retroarch_verbose.log` for
   `[GL]: Vendor: Mesa, Renderer: Mali-G31 (Panfrost)`.

Root password: `retroopi` (development credential).
