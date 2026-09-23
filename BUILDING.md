# Building RetroOPI_Z3

The repo holds sources, patches, configs and docs. It does **not** hold build
outputs: `firmware/` is gitignored. Expect to build.

## Host requirements

| | |
|---|---|
| OS | Linux, or Windows with WSL2 (developed on Ubuntu 22.04) |
| Disk | **~30 GB** for the build tree. LLVM and clang are part of it |
| RAM | 16 GB recommended. LLVM links are memory-hungry |
| Time | Several hours for a first build: toolchain, LLVM/clang, kernel, TF-A, U-Boot, Mesa, RetroArch, 21 cores |
| Network | Required on first build (~1.5 GB of sources) |

Buildroot's prerequisites (Debian/Ubuntu):

```bash
sudo apt-get install -y build-essential git bc bison flex libssl-dev \
    libncurses-dev python3 python3-dev unzip rsync wget cpio file \
    gawk gettext texinfo help2man libtool-bin pkg-config
```

## Building

`scripts/build.sh` does it all. It clones Buildroot 2026.02.3 into
`~/opi/buildroot` on first use, and builds out-of-tree into `~/opi/output`:

```bash
bash scripts/build.sh              # full image
bash scripts/build.sh defconfig    # (re)apply the defconfig only
bash scripts/build.sh linux-rebuild
```

Or by hand:

```bash
cd ~/opi/buildroot
BR2_DL_DIR=~/bpi/dl make BR2_EXTERNAL=/path/to/RetroOPI_Z3/buildroot-external \
    O=~/opi/output opi_zero3_retro_defconfig
BR2_DL_DIR=~/bpi/dl make O=~/opi/output -j$(nproc)
```

The result is `~/opi/output/images/sdcard.img`. See `docs/flashing.md`.

`buildroot-external/` and `launcher/`, `tools/` must stay siblings: the
launcher and tools packages build from `$(BR2_EXTERNAL_RETROOPI_PATH)/../`.

## What you must supply

- **ROMs**, on the FAT32 `RETROROMS` partition, one folder per system.
- **PlayStation BIOS** (`SCPH1001.BIN`) in `_system/bios/` on that partition.
- **A USB Bluetooth dongle** if you want a wireless DS3. The onboard radio is not enabled yet.

## Things that will bite you (most inherited from the reference)

- **Kconfig drops things silently.** After changing the defconfig, check that
  every line made it into `~/opi/output/.config`. panfrost silently vanished
  until `BR2_PACKAGE_MESA3D_LLVM=y` was added.
- **`target/` is incremental.** Deleting or renaming an overlay file does not
  remove it from the image. `board/opi-zero3/post-build.sh` deletes known
  stale paths and hard-fails on invariants. Add to it.
- **The kernel version is tied to the vendored patches.** `patches/linux/` is
  Armbian's set for exactly 6.18.53. Bump both together, and re-run the apply
  test described in that directory's README.
- **Patches live in two places:** `package/retroarch/libretro-*/` for cores,
  and `board/opi-zero3/patches/<pkg>/` (`BR2_GLOBAL_PATCH_DIR`) for linux,
  mesa3d, bluez5_utils and fuse.
- **Line endings.** `.gitattributes` forces LF. A CRLF in a script, patch or
  init script fails in ways that look like a different bug.
- **Verify a change reached the image,** not just the source. `board.sh push`
  prints board and host md5 side by side for that reason.
