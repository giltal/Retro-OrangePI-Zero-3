# RetroOPI Z3

Retro gaming console firmware for the **Orange Pi Zero3** — Allwinner H618,
four Cortex-A53, Mali-G31 MP2, 2 GB — on **HDMI at 720p**.

Buildroot 2026.02.3 and mainline Linux 6.18.53, plus Armbian's H616 HDMI
patches. It boots straight into a custom DRM/KMS launcher. RetroArch runs on
the GPU through Mesa panfrost, with the reference project's 21 emulator cores.

A port of [RetroBPI_M2M](https://github.com/giltal) (Banana Pi M2 Magic, DSI
panel), which was itself a port of the LyraZeroW SuperRetroPack.

> **Status: working.** Boots to the launcher on HDMI 720p. NES, PlayStation and
> Nintendo 64 run smoothly on the GPU (RetroArch reports `Mali-G31 MC1 (Panfrost)`,
> OpenGL ES 3.1), with HDMI audio and a DualShock 3. PSP is playable, including
> heavy titles like Assassin's Creed Bloodlines (with auto frameskip). Dreamcast runs
> at 91-95% speed in Sonic Adventure 2. The GPU runs at up to 600 MHz. The remaining
> systems are built but not individually tested. See `DevelopmentLog.md`.

---

## What it does

| | |
|---|---|
| **Systems** | NES, SNES, Game Boy / Color / Advance, Genesis, Master System, Game Gear, PC Engine, SuperGrafx, Atari 2600 / 7800 / 800 / 5200, ZX Spectrum, Neo Geo / CPS / arcade, Doom, PlayStation, Nintendo 64, **PSP** (PPSSPP), **Dreamcast** (Flycast) |
| **Display** | HDMI 1280×720 (1080p planned). The launcher picks the matching TV mode and scales its layout to it |
| **GPU** | Mali-G31 via Mesa panfrost, GLES 3.1. RetroArch `gl` driver on KMS/GBM |
| **Input** | DualShock 3, wired, or over Bluetooth via a USB dongle |
| **Audio** | HDMI, with a software volume control the launcher and in-game hotkeys share |
| **Launcher** | Jump-to-letter, favourites, recents, save-state slots, search, themes |

## Why the kernel carries patches

Mainline Linux still has no HDMI for the H616/H618. The display engine driver
is there; the HDMI PHY, the TV timing controller and every display node are
not. Armbian carries Jernej Škrabec's refactor, which does all of that. The 49
patches here are vendored from Armbian unmodified, pinned to one commit. See
[`buildroot-external/board/opi-zero3/patches/linux/README.md`](buildroot-external/board/opi-zero3/patches/linux/README.md).

## Building

See **[BUILDING.md](BUILDING.md)**. In short: WSL2 or Linux, ~30 GB of disk,
a few hours. LLVM is part of the build, because Mesa's panfrost needs it. You
supply your own ROMs and PlayStation BIOS.

## Repository layout

```
buildroot-external/   BR2_EXTERNAL: defconfig, package recipes, board files, patches
launcher/             the launcher (C, DRM/KMS + SDL2_ttf)
tools/                volumed, inject-input, seed-credit
scripts/              build.sh (WSL build), board.sh (deploy / inspect over SSH)
Context.md            hardware, differences from the reference, plan
DevelopmentLog.md     how it was built, including the wrong turns
```

## Licence

GPL-2.0-or-later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Buildroot, Linux, U-Boot, TF-A, Mesa, RetroArch and the libretro cores are
fetched from their own upstreams at pinned revisions and remain under their
own licences. No ROMs or BIOS images are distributed here.
