# Development Log — RetroOPI_Z3

Newest entries at the bottom. Record the wrong turns as well as the fixes;
the reference project's log showed they are often the more useful half.

---

## 2026-09-23 — Session 1: project scaffold, kernel base, first build

### Board identification (a near miss)

The request first named the board as the "Orange Pi Zero 3W". That board is real, and it is
an **Allwinner A733** with an **Imagination PowerVR BXM-4-64** GPU. Mainline A733 support is only
at the pinctrl/clock/DMA stage, with no display and no GPU. The only working route there is Orange
Pi's vendor 6.6 BSP plus closed PowerVR userspace grafted from Radxa images. That would have been
a completely different project. The user corrected it: the board is the **Orange Pi Zero3,
2 GB, H618**. Worth writing down, because the names are one letter apart.

### Kernel: HDMI is not in mainline

Mainline 6.18 has the DE33 mixer, but none of the H616 display nodes, no H616 HDMI PHY variant
and no TCON-TV support. A DT-only upstream attempt was NACKed on linux-sunxi this month in favour
of Jernej Škrabec's `sun4i-drm-refactor`, which is not merged.

Armbian carries that refactor and ships HDMI on this board. Decision: **mainline 6.18.53 + a
subset of Armbian's `sunxi-6.18` patches, vendored unmodified** at commit `2843c11a` (2026-09-22,
"rewrite patches against 6.18.53"). The kernel version is pinned to match.

Apply-tested on a pristine 6.18.53 tree, in `series.conf` order:
- `patches.drm/0001-0042`: all apply.
- `0701` (H616 AHUB audio driver, which carries HDMI audio) and `0702` (the HDMI audio clock fix;
  without it the PCM runs silent): apply.
- BL31 reserved-memory, Zero3 CPU DVFS, Zero3 GPU enable, nvmem-H616 and SRAM-C1: apply.
- `fixes-6.18/0014` **fails**. It fixes an out-of-tree megous patch we don't carry. Dropped.
- `drv-thermal-sun8i-guard-against-null-caldata` **fails** on its A523 hunk and is defensive
  only. Dropped.
- `drv-drm-sun4i-hdmi-add-audio-support`: **not taken**. It is for the A10/A20 HDMI block.

The final 49 are renumbered 0001–0049 in `patches/linux/`, re-verified in glob order. Provenance
is in `patches/linux/README.md`.

### Board layer

Modelled on Buildroot's own `orangepi_zero3_defconfig`: AArch64, arm64 `defconfig` + our
fragment, TF-A `sun50i_h616`, U-Boot `orangepi_zero3`. Versions follow what Armbian ships on this
board: TF-A `lts-v2.12.9` (needs the 512K BL31 reservation, patch 0045) and U-Boot `v2026.07`.

### Mesa: panfrost silently dropped

After the first `defconfig`, `.config` had no panfrost, and so no GBM, EGL or GLES either.
There was no error. In Mesa 26, panfrost's shader library is precompiled from OpenCL C, so
Buildroot makes the driver depend on `BR2_PACKAGE_MESA3D_LLVM`. With that unset, Kconfig removes
the driver, then everything that needs a driver. Fixed by adding `MESA3D_LLVM=y`. This adds
LLVM, clang and libclc to the build.

**Process fix:** after any defconfig change, check that every `BR2_*` line in the defconfig
appears verbatim in `.config`. The check found nothing else missing.

### Carried over from RetroBPI_M2M

- `package/retroarch/**`: RetroArch patches 0001–0006 (KMS node scan, primary-plane handling) and
  all core packages. **0007 (panel rotation) dropped**, along with its build plumbing.
- launcher, tools, init scripts, DS3/BlueZ config and patch, fuse patches, mesa `dril` patch,
  retroarch.cfg and per-core configs, and the post-build guards.
- **Not carried:** DSI/panel/touch kernel patches and DTS, AP6212 firmware, `S05powercap`
  (AXP223 900 mA workaround), the A33 `asound.state`, the Mali-400 overclock.

### Changes for HDMI

- **Launcher mode selection.** The launcher took `conn->modes[0]`, the display's *preferred*
  mode. On the panel that was the only mode. On a TV it is typically 1080p or 4K, while the
  framebuffer is 1280×720, so `setCrtc` would fail. It now picks the mode matching
  `SCREEN_WIDTH×SCREEN_HEIGHT`, preferring progressive 60 Hz, and lists the offered modes if there
  is no match. It also polls up to 10 s for hot-plug, because TVs raise HPD late.
- **Launcher layout scaling.** Every size was tuned at 480 lines. `UI_SCALE(px)` scales by
  `SCREEN_HEIGHT/480` (1.5× at 720p, 2.25× at 1080p). It was applied to fonts, layout constants
  and about 70 hard-coded offsets in the header, footer, list, slot picker and search keyboard.
  Done with a script that asserted an exact match count per replacement. One count was wrong on the
  first run, so the script aborted and nothing was written. **Needs eyes on real HDMI.**
- **Volume.** HDMI has no analog stage, and the AHUB's controls are patch-set specific. S11alsa
  generates `/etc/asound.conf`: `default` → plug → **softvol 'Master'** (101 steps, 0 = mute,
  1–100 = −40–0 dB) → the card whose name contains "hdmi". Index == percent, so the launcher
  and volumed share one control with no scale translation. RetroArch `audio_device` goes from
  `hw:0,0` to `default`. It moved from S35 to **S11** because the launcher (S12) sets the volume at
  startup, and softvol only creates its control on first open. S11 primes it with 0.1 s of silence.
  post-build now enforces that the alsa script sorts before the launcher.
- **Heredoc backslash trap (again).** Generating the launcher's `volume_query()` through an
  inline Python heredoc turned `\\(` into `\(` inside a C string. That compiles, with a warning,
  into a `sed` expression that matches nothing, so the volume would have read back as "keep the old
  value" forever. It was caught by diffing that line against the reference, and fixed with a literal edit.
- **btusb as a module.** Realtek USB BT dongles need firmware, and built-in btusb would probe
  before the rootfs mounts. That is the reference's brcmfmac lesson, applied before it could bite.

### Open, to verify on hardware

- Does HDMI come up at 720p? Is the launcher legible and correctly laid out?
- Card name of the AHUB HDMI device (S11alsa matches `hdmi`, case-insensitive). Does sound
  actually come out, not just a RUNNING PCM? Listen.
- RetroArch KMS context on DE33: mode set, `[GL] Renderer: Mali-G31`.
- The vsync-off reasoning assumed sun4i-drm cannot async-flip. Re-check on DE33.
- Temperatures under N64 load at the `performance` governor.

### Core audit for AArch64

On aarch64, `LIBRETRO_PLATFORM` comes out as `"buildroot gles armv8"`. There is no `neon`,
because Buildroot only sets NEON for cortex_a53 in 32-bit mode, and no `hardfloat`. Most libretro
Makefiles treat any `armv` in the platform string as **32-bit ARM**: `-marm`, `-mfpu`, and the
ARMv7 JIT. And `platform=unix` is not a safe default either: some Makefiles then pick
dynarecs from the *host's* `uname`, which would give an x86 JIT.

Every core was audited against its Makefile at the pinned commit and cross-built in a
throwaway output (`~/opi/coretest`). All 21 end up as AArch64 `.so` files, and no build log
contains `-marm`, `-mfpu` or `-mfloat-abi`.

| Core | Result |
|---|---|
| gpsp | **fixed**: `platform=arm64`, which builds the arm64 JIT (`arm64_stub.o`, `-DARM64_ARCH -DHAVE_DYNAREC`) |
| nestopia, genesisplusgx | **fixed**: `platform=unix` (they took the armv/`rpi3` 32-bit flags) |
| beetlesupergrafx | **fixed**: `platform=unix IS_X86=0` (IS_X86 came from the host's `uname -p`) |
| fuse | **fixed**: `platform=unix` (the armv branch also forced `CC=gcc`, the *host* compiler) |
| prboom | **fixed**: `platform=unix` |
| fbalpha2012 | **fixed**, not arch-related: it was missing its `zlib` dependency, so a clean build failed on `-lz` |
| pcsx_rearmed | OK: detects the target via `$(CC) -dumpmachine`, gives `ari64` dynarec + `gpu=neon` |
| parallel-n64 | OK: `ARCH=aarch64` gives new_dynarec arm64 (`linkage_aarch64.o`), GLES2 |
| everything else | OK as-is (the reasons are recorded in the audit, and in comments where the .mk was touched) |

**Consequence for build #1:** it had already parsed the old `.mk` files when these fixes
landed. The seven fixed cores must be `dirclean`ed and rebuilt from build #1's output.

Latent, not fixed: `libretro-paralleln64.mk` keys `FORCE_GLES=1` on `BR2_PACKAGE_HAS_LIBEGL`. With
EGL off it silently tries desktop GL, and fails on `GL/gl.h`. Not reachable with this defconfig.

### Build #1

Started 08:37. Status: *in progress* (see below).
