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

Started 08:37. **Failed after 103 min in target clang** (`clangSema`):
`fatal error: Killed signal terminated program cc1plus`. dmesg confirms the OOM killer.
`build.sh` ran a top-level `make -j20`, so Buildroot built several packages at once, each with
about 20 jobs. The core-audit test builds were also running at the time. WSL has 15 GB.

Fix: `build.sh` now runs one package at a time with `BR2_JLEVEL` jobs inside it (default
nproc, override with `JLEVEL=`). Resumed with clang at `JLEVEL=10`, then the rest at full width.
Upside: no core had been built yet, so the audit's `.mk` fixes apply without any dirclean.

Resume: clang finished in 2 min at JLEVEL=10. Next failure, 17 min later, was **U-Boot 2026.07
host tools**. `mkeficapsule` now calls `gnutls_pkcs11_*`, and Buildroot's host-gnutls has no
p11-kit, so the link fails. EFI capsule updates are irrelevant to an SD-booted console:
`# CONFIG_TOOLS_MKEFICAPSULE is not set` in `uboot-fastboot.config`, then `uboot-dirclean`
so the fragment is re-merged. The resume script verifies the option is actually off in U-Boot's
`.config` before continuing.

The full build then **succeeded** (11:04, sdcard.img 3.2 GB). Before calling it done I
verified the artifact, not just the exit code:
- all 21 cores are AArch64. The launcher, volumed and retroarch are aarch64. RetroArch links
  libgbm/libEGL/libGLESv2
- Mesa installed `panfrost_dri.so` **and** `sun4i-drm_dri.so`, so the kmsro path is complete
- DTB: `display-engine`, DE33 `bus@1000000`, `tcon-top`, `tcon-tv` (`lcd-controller@6515000`),
  `hdmi`, `hdmi-phy` and `gpu` are all enabled. (The disabled `lcd-controller@6511000` is the LCD
  TCON, which is unused.)
- post-build guards passed. The MBR has p1 ext4 1 GiB and p2 FAT32 2 GiB.

**But the kernel config was not what the fragment asked for.** Comparing every fragment line
with the kernel's `.config` found 9 mismatches. All of ALSA came out `=m` because arm64
defconfig sets the parent `CONFIG_SOUND=m`, and `BT=m` because of `RFKILL=m`. Kconfig caps
children at their parent's value and says nothing. Modules would probably have worked via udev,
but the image did not match its own documentation. Fixed with `CONFIG_SOUND=y` and
`CONFIG_RFKILL=y`, and **post-build.sh now fails the build** when any fragment line does not
survive into the kernel `.config`. That makes this the third silent-Kconfig catch today
(panfrost, sound, BT), which is the reason for a guard rather than a note. `linux-dirclean`,
then rebuild.

**Build #1 final: 11:22, `sdcard.img` md5 `63a72d5fbcc92a907103997833ddeb13`** (3.2 GB, copied to
`firmware/`, md5 identical). Zero fragment mismatches. `SND`, `SND_SOC_SUNXI_AHUB`,
`DRM_DW_HDMI_I2S_AUDIO`, `BT` and `RFKILL` are all `=y`. The new guard was tested against a
deliberately wrong fragment line, and it fails the build as intended.

Total wall time ~2 h 45 min, including the OOM and the U-Boot detour.

### First boot of build #1: black HDMI, not on the network

The user reports the board "booting" (power on), with **nothing on HDMI and no DHCP lease**. A
sweep of 10.100.102.0/24 found no `02:`-prefixed MAC (the address U-Boot derives from the SID) and
no dropbear. The one SSH host, .51, is OpenSSH/Debian, the user's Pi Zero 2. No network rules out
"just a display problem". The board never reached userspace, or never got the link up.

The boot chain was checked offline and is sound: `eGON.BT0` at 8 KB and byte-identical to the build,
TF-A BL31 embedded, U-Boot with the Zero3 LPDDR4 DRAM settings, bootstd/extlinux/ext4, and `/boot`
holding Image + DTB + a matching extlinux.conf.

**Self-inflicted blind spot:** the image booted with `quiet loglevel=4`, and U-Boot has no HDMI
on the H616. So even a kernel that reached fbcon would show a black screen. Rebuilt as a
**bring-up image**: `earlycon console=tty1 console=ttyS0,115200 loglevel=7`, plus a `tty1` getty
appended to inittab by post-build.sh. (BusyBox inittab has no inline comments; the first version
put one after the command, which would have been passed to getty as arguments. Caught before
flashing.) md5 `6a813d5ddabe4028f2c6535c5baeb3a1`.

Waiting on: serial output, LED behaviour, and whether the TV says "no signal" or shows black.

**Serial (COM8) settled it: the SPL never gets past DRAM init.** Looping forever:

    U-Boot SPL 2026.07 (Sep 23 2026 - 10:43:21 +0300)
    DRAM:This DRAM setup is currently not supported.
    resetting ...

That panic is at the end of `mctl_auto_detect_rank_width()` in the new `dram_dw_helpers.c`
(Jernej Škrabec, 2025). It tries 32/16-bit × rank 2/1, and `mctl_core_init()` fails for all four.
So DRAM **training** fails outright; this is not a size-detection problem. Our U-Boot `.config` DRAM
values are identical to upstream `orangepi_zero3_defconfig` (LPDDR4, 792 MHz, TPR6 0x44000000,
TPR11 0x24242624, TPR12 0x0f0f100f). Armbian carries no Zero3 DRAM patches on v2026.07. The
Zero 2W (same SoC and RAM family) uses TPR6 0x48808080, TPR11 0x26262524, TPR12 0x100f100f.

> **RETRACTED: wrong board.** The board on the bench was an **Orange Pi Zero2** (H616; it looks
> almost identical), not the Zero3. So the DRAM failure above says nothing about the Zero3 image,
> and neither do the "black HDMI / no network" results before it. With the actual Zero3, build #1
> (the verbose-console image) **booted fully**: launcher on HDMI, DS3 working, on the network at
> 10.100.102.85 (MAC 02:00:87:ff:1a:4a, dropbear). The A/B bootloader images below are therefore
> moot and can be deleted.
>
> Lesson: when a board shows no sign of life, confirm **which board** is connected before
> debugging the image. The serial banner told us the SoC failed DRAM training, which should have
> prompted "is this even a Zero3?". The H616 and H618 SPLs are identical at that stage.

Two bootloader-only test images, built outside Buildroot in `~/opi/ubtest` with the same BL31
and dd'd over build #1's image at 8 KiB. The partitions are untouched, as checked with sfdisk:
- `sdcard-A-v2025.04.img`: U-Boot v2025.04, stock zero3 config. That is Armbian's long-time Zero3
  pin, from before the DRAM-helper rework. **If A boots, it's a v2026.x regression.**
- `sdcard-B-v2026.07-z2wtpr.img`: v2026.07 with the Zero 2W TPR6/11/12. **If only B boots, this
  board's LPDDR4 wants different timings.**

### Zero3, first real boot

The launcher is up on HDMI and the PS3 pad works. **Launching a game returns straight to the
launcher.** Board at 10.100.102.85. The dev SSH key `~/.ssh/retroopi_ed25519` was installed by the
user (I don't type the root password myself). `board.sh status`: HDMI connected, ALSA card 0 =
"H616 Audio Codec", card 1 = "HDMI". Thermals ~50 °C idle. `performance` governor at 1416 MHz.
rcS done at 7.2 s.

RetroArch log:

    [WARN] [KMS]: Couldn't create GBM device.
    [ERROR] [KMS]: Couldn't find a suitable DRM device.
    [ERROR] [Video]: Cannot open video driver.. Exiting..

It looked like the reference's GBM/dril problem, but it is not: `sun4i-drm_dri.so`,
`panfrost_dri.so` and `/usr/lib/gbm/dri_gbm.so` are all present. The real cause is that
**there is no GPU at all**. `/dev/dri` has only `card0` (sun4i-drm), no render node, and dmesg shows

    panfrost 1800000.gpu: deferred probe timeout, ignoring dependency
    panfrost 1800000.gpu: probe with driver panfrost failed with error -110

The gpu's device-link suppliers: the PMIC (i2c 0-0036) and the CCU are `available`, and
**`7010250.power-controller` is `dormant`**. That's `allwinner,sun50i-h616-prcm-ppu`, driven by
`CONFIG_SUN50I_H6_PRCM_PPU`. Its Kconfig help reads "required to enable the Mali GPU in the H616
SoC". arm64 defconfig has only `SUN20I_PPU` (the D1 one), which is `default ARCH_SUNXI`; the H6/H616
one has no default. Added `CONFIG_SUN50I_H6_PRCM_PPU=y` to the fragment.

Also found: the launcher's startup banner still said "RetroBPI / Banana Pi BPI-M2 Magic". The
session-1 Python replacement for it had no match-count assertion, and its `\n` escaping didn't
match, so it silently did nothing. Fixed with a literal edit. (The same backslash trap as before, and
the second time a replacement without an assertion has hidden a miss.)

`board.sh kernel` now also pushes `/lib/modules/<ver>` (wholesale, keeping `.prev`), so kernel
fixes can be deployed without reflashing.

Deployed with `board.sh kernel` (board and host Image md5 match, `9bef92eb…`) and `board.sh push
/usr/bin/retroopi_launcher`, then `reboot` over SSH. **The board did not come back on the network**
(>2 min, no ping). COM8 was silent too, apparently still wired to the Zero2 from earlier. The user
power-cycled it.

### Result: games run

After the power cycle, reported by the user: **NES games run, the DS3 works, search works, and
HDMI sound works.** So the whole chain works on real hardware: launcher (720p, UI_SCALE) →
RetroArch `gl` over KMS/GBM on panfrost → HDMI audio via the AHUB + softvol `default`.

Open items from this session:
- **The SSH `reboot` did not return by itself.** It's unknown whether it hung in shutdown (S12launcher
  stop, umount) or in the next boot. Needs the serial console on the Zero3 to see.
- **After the power cycle the board was not on the network** (no DHCP / no `02:00:87:ff:1a:4a` in
  ARP), while the launcher, games and sound all worked. Check the cable first, then treat it as a bug:
  S40network backgrounds `ifup`, and the hot-plug rule should catch a late link.
- Not yet verified on the board: the renderer string (`Mali-G31 (Panfrost)`), panfrost devfreq, and
  the S11alsa card choice (card 1 "HDMI" expected).
- The A/B DRAM test images in `firmware/` (from the Zero2 detour) can be deleted.
