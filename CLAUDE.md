# RetroOPI_Z3 — Project Instructions

## What This Is

Retro gaming console firmware for the **Orange Pi Zero3, 2 GB** (Allwinner **H618**, 4× Cortex-A53
@ 1.5 GHz, **Mali-G31 MP2**), output over **HDMI at 1280×720** (1080p later).

It is a port of **RetroBPI_M2M** (`C:\BananaPi_Projects\RetroBPI_M2M`), a Banana Pi M2 Magic with
a DSI panel. The userspace carries over: the launcher, RetroArch and 21 cores, the init scripts,
DualShock 3 over Bluetooth, and the volume and clean-shutdown handling. The board layer (kernel,
display, audio) is new.

- **Read `Context.md` first.** It covers the hardware, what differs from the reference, and the plan.
- **Read the last entry of `DevelopmentLog.md`** before continuing work.
- **Consult the reference's `DevelopmentLog.md`** before changing a core choice or a RetroArch
  setting. Most settings here were measured there, and the log says why.

> Note: this is the **Zero3 (H618)**, *not* the "Zero 3W", which is an Allwinner A733 with a
> PowerVR GPU and needs a completely different (vendor-BSP) approach.

## Hard constraints

- **Mainline Linux has no H616/H618 HDMI** (as of 6.18, and still in 2026-09). The HDMI, TCON-TV
  and DE33 support comes from Armbian's patch set, vendored under
  `buildroot-external/board/opi-zero3/patches/linux/`. Its `README.md` records the provenance and
  the exact Armbian commit. **The kernel version is pinned to that patch set** (6.18.53); bump them
  together.
- **panfrost needs LLVM** in Mesa 26 (`BR2_PACKAGE_MESA3D_LLVM=y`). Without it Kconfig silently
  drops panfrost, GBM, EGL and GLES.
- The **onboard Wi-Fi/BT (UWE5622)** needs an out-of-tree driver and is **not enabled**. Networking
  is the onboard Ethernet. Bluetooth for the DS3 needs a USB BT dongle until the UWE5622 is brought in.
- **Serial console:** the 3-pin debug header, 115200 8N1, 3.3 V. Keep it attached during bring-up.

## Development Environment

- **Host OS:** Windows + WSL2 (Ubuntu 22.04). The layout is kept apart from the reference project's `~/bpi`:
  - `~/opi/buildroot`: Buildroot 2026.02.3
  - `~/opi/output`: the build (`O=`)
  - `~/bpi/dl`: download cache, **shared** with the reference on purpose
  - `~/opi/armbian-build`: sparse checkout of Armbian's sunxi-6.18 patches, the source of `patches/linux`
- **Do not modify anything under `~/bpi`** except the shared `~/bpi/dl`. That is the reference project's tree.
- **WSL command pattern:** `wsl bash /mnt/c/.../script.sh` from PowerShell. Put anything
  non-trivial in a script file. `wsl bash -c '...'` from git-bash mangles `$vars`, pipes and `/mnt/c` paths.
- **Backslashes get eaten one level** when generating files through heredocs or inline Python.
  This bit this project on day one (a `sed` escape inside a C string in the launcher). Use the
  Edit tool for any line that contains a backslash, and diff against the reference afterwards.
- **Line endings:** `.gitattributes` forces LF. After writing any shell script, check with
  `grep -rlI $'\r' .`.

## Build

```bash
wsl bash /mnt/c/OrangePI_Projects/RetroOPI_Z3/scripts/build.sh              # full image
wsl bash /mnt/c/OrangePI_Projects/RetroOPI_Z3/scripts/build.sh defconfig    # re-apply defconfig
wsl bash /mnt/c/OrangePI_Projects/RetroOPI_Z3/scripts/build.sh linux-rebuild
```

The log is `~/opi/build.log`. The result is `~/opi/output/images/sdcard.img`, copied to `firmware/`.

**After editing the defconfig**, confirm every line survived into `~/opi/output/.config`. Kconfig
drops symbols whose dependencies are unmet **without any error**. That is how panfrost went
missing.

## Board

`scripts/board.sh` does status, push, kernel, run and ssh over Ethernet. Write the board IP to
`~/opi/board_ip` once. Root password `retroopi` (a development credential).

## Key Paths

```
C:\OrangePI_Projects\RetroOPI_Z3
  Context.md, DevelopmentLog.md, BUILDING.md
  buildroot-external/
    configs/opi_zero3_retro_defconfig
    board/opi-zero3/
      linux-retrogaming.config     kernel fragment on arm64 defconfig
      patches/linux/               49 Armbian patches (HDMI, audio, GPU enable) + README.md
      patches/mesa3d/              dril-for-headless-GBM (needed for kmsro)
      rootfs_overlay/              init scripts, retroarch.cfg, BT config
      post-build.sh                target/ invariants -- keep adding guards here
    package/retroarch/             RetroArch + libretro cores (from the reference)
    package/retroopi-launcher/     launcher package (SCREEN_W/H)
  launcher/                        the launcher (C, DRM/KMS + SDL2_ttf)
  tools/                           volumed, inject-input, seed-credit
  scripts/                         build.sh, board.sh
  firmware/                        build output (gitignored)
```
