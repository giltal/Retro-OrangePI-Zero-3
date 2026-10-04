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
- The A/B DRAM test images in `firmware/` (from the Zero2 detour) can be deleted. *(Done.)*

### Verified on the board (network back, same IP .85)

- panfrost probes at 2.2 s: `mali-g31 id 0x7093`, `/dev/dri/renderD128` present, GPU at
  **432 MHz fixed**. There is no devfreq entry because the H616 DT has no GPU OPP table.
  Possible future tuning.
- RetroArch (NES, 8 s run over SSH):
  `[GL]: Found GL context: "kms"`, `Renderer: Mali-G31 MC1 (Panfrost)`,
  `Version: OpenGL ES 3.1 Mesa 26.0.1`.
- S11alsa chose **card 1 "HDMI"** (`ahub_plat-i2s-hifi`). Card 0 is the analog "H616 Audio Codec".
- rcS finished at 5.35 s.

**Bug: the volume control never worked.** `amixer: Cannot find the given element`. softvol
uses its `control.name` **verbatim**. S11alsa said `name "Master"`, which created a control
literally named `Master`, while the launcher, volumed and S11alsa's own check all ask for
`'Master Playback Volume'`. Sound played at the default level and every volume change went nowhere.
S11alsa even logged `FAIL (no Master control)` at boot, but nobody reads the boot log. Fixed the name
and added a post-build guard: the name S11alsa creates must appear in the launcher and volumed
binaries. Verified on the board: set/get works and `amixer sset Master` sees it. Restored the level to
the launcher's saved 40%.

### ROM partition sized for a 32 GB card

The user flashes 32 GB cards, and 2 GiB of ROM space was far too small. `post-image.sh` now defaults
`ROMS_SIZE_MB` to **27648 MiB**, making the image 28673 MiB = **30.07e9 bytes**. "32 GB" cards
really hold ~30.5e9–32.0e9 bytes depending on brand, so this fits all of them at the cost of up to
~2 GB. It can be overridden with `RETROOPI_ROMS_SIZE_MB`.

- `roms.vfat` is now made with `truncate` (sparse) instead of `dd if=/dev/zero`. genimage kept the
  holes: 29 GB apparent, 550 MB on disk. Image generation stays about 1 min.
- `build.sh` now delivers **`firmware/sdcard.img.xz` (126 MB)**. Etcher and Rufus flash `.xz`
  directly. The raw `.img` is only copied to `/mnt/c` when it is ≤ 4 GiB, because NTFS would
  materialise every hole.
- Verified: MBR p1 1 GiB / p2 27 GiB (type 0x0c). `fsck.fat -n` is clean. FAT32 with 16 KiB clusters,
  28,976,381,952 bytes free, the system folders and README present. `xz -t` passes, and the
  decompressed stream's md5 equals the built image (`0f6dd6e4…`).

**Follow-up: flashing time.** The 32 GB image works, but the flashing tool writes all 30 GB,
where the old image took about a minute. The user prefers to flash a small image and grow the FAT
partition on Windows. `build.sh` gained `IMAGE_TAG`, so variants sit side by side:
`RETROOPI_ROMS_SIZE_MB=2048 IMAGE_TAG=small` gives `firmware/sdcard-small.img` (3.2 GB raw, p2 =
2 GiB, md5 `9efc4c79…`) plus `.xz`, next to the 32 GB `sdcard.img.xz`, which was kept unchanged. Note:
Windows Disk Management cannot extend FAT32 (NTFS only). It needs a third-party tool (MiniTool /
AOMEI). p2 is the last partition, so growing it in place is safe.

**User test: N64 and PSX run smoothly** (by eye and ear, not yet measured). These are the two
heavy systems. On the reference, N64 only reached real-time at 320x240 with a tuned `.opt`. It still
runs with that same inherited `.opt` here (parallel-n64, rice, 320x240), so this is the floor, not the
ceiling. Next, measured the reference's way (vblank-counted fps, the audio-seconds/wall-seconds
ratio, under gameplay rather than attract mode): 640x480 via Settings → N64 Quality, and
mupen64plus-next (GLES3) as a candidate.

**N64 at 640x480:** the user switched Settings → N64 Quality to 640x480 and reports N64 smooth
(by eye). On the reference this ran at ~91% speed on the geometry-bound Mali-400. It is now the
shipped default in `ParaLLEl N64.opt`, and the toggle can still switch back to 320x240.

### 1080p

`SCREEN_W/H` → 1920×1080 in the defconfig and `video_fullscreen_x/y` → 1920/1080 in retroarch.cfg.
A new post-build guard fails the build if the two differ, because a mismatch means a full HDMI
re-sync on every game launch and exit. Deployed without reflashing (`board.sh push` of the launcher and
retroarch.cfg): the launcher reports `DRM: mode 1920x1080@60`, ready in 276 ms, ~0% CPU idle.
UI_SCALE is 2.25× at this height.

The card had been reflashed (to the 32 GB image), which wiped `authorized_keys`, so board.sh was
locked out until the key was installed again by hand. **post-build.sh now bakes the build host's
`~/.ssh/retroopi_ed25519.pub` into the image** when that file exists. It never enters the repository,
and images built elsewhere get no key.

**1080p broke game launch.** RetroArch initialised GL fine at 1920x1080 and then died (status -1):

    DRM_IOCTL_MODE_CREATE_DUMB failed: Cannot allocate memory
    MESA: error: Failed to create scanout resource

Scanout buffers come from CMA, and arm64 defconfig reserves **32 MiB**, with `CmaFree` only 6 MB while
the launcher was idle. One 1080p XRGB buffer is 8 MiB: fbcon holds one, the launcher double-buffers,
and RetroArch/Mesa needs ~3 more. At 720p it all fitted by luck (2.25x smaller). Fixed with
**`cma=256M`** on the kernel command line (extlinux.conf, deployable without a kernel rebuild), plus a
post-build guard that extlinux carries a `cma=`. User: **games run at 1080p**.

**Networking after reboot: FIXED.** After the `reboot` that applied cma=256M, the board was again
absent from the network while the user played games on it. The board reboots fine, but **Ethernet
does not survive a warm reboot**. The user confirmed it: only a hard power cycle brings the network
back.

Two of my earlier checks had been invalid and should not have been used as evidence. One wait loop was
piped through PowerShell's `Select-Object -First 3`, which killed it after three lines. The other
ran immediately after the reboot. A proper test (poll for 180 s) confirmed: not back.

Root cause (a known Zero3 problem, see the DietPi forum): `S40network stop` ran `ifdown -a`. Bringing
eth0 down ends in `phy_suspend()` → `BMCR_PDOWN`, which powers the YT8531 PHY off. The board has
no PHY reset line and the PHY supply stays on across a warm reset, so the next kernel's EMAC reset
waits for a clock that never comes. **Fix:** `stop` now only releases the DHCP lease (udhcpc SIGUSR2)
and stops the clients, leaving eth0 up. Two warm reboots: back after 21 s and 19 s, link up at ~8.4 s,
same lease.

### Launcher visuals: name and symbols

- Header "LYRA LAUNCHER" → **"RETRO LAUNCHER"**.
- Every UI symbol (⚙ ★ ⏱ ◀ △ □ ✕) rendered as a "missing glyph" box: `launcher.ttf` has **none** of
  them. This was checked by parsing its `cmap` directly with stdlib Python (no fontTools in WSL), and the
  same check showed DejaVu Sans covers all of them except U+23F1 ⏱. Fix: `BR2_PACKAGE_DEJAVU`
  (sans only) plus a per-glyph fallback in `gfx_draw_text()`. The text is split into runs by
  `TTF_GlyphIsProvided32()`, and each run is rendered with its own font and aligned on a common
  baseline via `TTF_FontAscent()`. Recently Played uses ◷ (U+25F7) instead of ⏱. A post-build guard
  checks the font file is present. Also: two `-Wempty-body` warnings (`if (system(...) == -1) ;`) fixed.
- An inline-Python edit of the launcher tripped the backslash trap again (`\\x` → `\x` inside the
  heredoc). The match-count assertion caught it and nothing was written. Done with the Edit tool.

### PSP (PPSSPP) and Dreamcast (Flycast), overnight

The user asked what else this SoC can run. The H618 is essentially the H700 of the Anbernic RG35XX
handhelds (4×A53 1.5 GHz + Mali-G31 MP2), where PSP is playable for many games at 1× and Dreamcast
often runs at full speed. Both need GLES3, which the reference's Mali-400 lacked.

- **RetroArch was built with `HAVE_OPENGLES3=no`**, so any core requesting a GLES3 context would
  fail. Added `--enable-opengles3 --enable-opengles3_1` when panfrost is enabled.
- **PPSSPP**: the old package pinned a 2018 commit of the libretro fork with Rockchip patches. It was
  replaced by upstream **v1.20.4** (git + submodules; the bundled FFmpeg has `linux/aarch64`
  prebuilts). With `-DLIBRETRO=ON -DUSING_GLES2=ON`, the libretro port skips the GL-core path and uses
  RetroArch's GLES3 context (`LibretroGraphicsContext.cpp`). The assets go to
  `/usr/share/ppsspp/PPSSPP` in the rootfs.
- **Flycast**: new package, **v2.7**, `-DLIBRETRO=ON -DUSE_GLES=ON`, no Vulkan, no OpenMP (no
  libgomp in our toolchain). The ARM64 dynarec (vixl) is selected from the architecture.
- **S46card** (new): brings an existing card up to date, strictly additively. It creates the missing
  `psp`, `dreamcast` and `_system/bios/dc` folders, and copies the PPSSPP assets into
  `_system/bios/PPSSPP` when a version stamp differs (backgrounded; never deletes anything, since
  PSP saves live in the same tree).
- **Launcher**: `psp` and `dreamcast` systems. **Subfolder games**: a folder holding a disc
  descriptor (m3u > gdi > cue > chd > cdi) is listed as one game under the folder's name. GDI sets
  *must* be laid out that way, because every GDI names its tracks track01.bin etc. In a flat folder
  containing a `.gdi`, the `.bin`/`.raw` tracks are hidden. `count_roms_in_dir()` counts game folders
  too, or a Dreamcast system holding only folders would be hidden as empty.
- **Save-state picker**: `core_display_name()` had no entries for PSX, N64 or GBA, so the slot picker
  never appeared for them. Added those plus PPSSPP and Flycast.
- **First build, first try: RetroArch (HAVE_OPENGLES3=1, 3_1=1), Flycast (33 MB) and PPSSPP (30 MB),
  all AArch64.** But built is not the same as working:
- **New tool, `tools/core-info`**: dlopens a core ON THE BOARD and prints its `library_name`,
  version, extensions and every core option with its values (via the legacy SET_VARIABLES path).
  First run: **both new cores failed to dlopen**. PPSSPP needed `libcpu_features.so` and Flycast
  needed `nowide.so.11.3.0`. Buildroot's cmake-package passes `BUILD_SHARED_LIBS=ON`, so vendored
  sub-libraries were built as `.so` files that are never installed. Fixed with
  `-DBUILD_SHARED_LIBS=OFF` in both. **New post-build guard:** every `NEEDED` entry of every core
  must exist in the target. It was tested against the broken target and fails as it should. In a
  game this would only have shown as "returns to the launcher".
- The same run confirmed the library_names for the new save-state entries: `gpSP`, `PCSX-ReARMed`,
  `ParaLLEl N64`.
- **Stale launcher, caught:** a full build shipped the OLD launcher binary, without the PSP/Dreamcast
  entries. `SITE_METHOD=local` packages are not re-synced on a plain `make` (a reference lesson,
  hit again). `build.sh` now dircleans `retroopi-launcher` and `retroopi-tools` before every full
  build. A post-build guard checks that every system folder in post-image.sh's `SYSTEMS` is present
  as a string in the launcher *binary*. Its first version used `strings` without `-n 2` and missed
  every folder name shorter than 4 characters (nes, gb, psp…); fixed.
- **Rebuilt statically, redeployed, and `core-info` on the board: both load.** `library_name` is
  `PPSSPP` (76 options) and `Flycast` (85 options). The version strings read `unknown` /
  `v0.0.0`, because both take them from git metadata the Buildroot tarball lacks. Cosmetic.
- **Core options** (`config/PPSSPP/PPSSPP.opt`, `config/Flycast/Flycast.opt`) were written ONLY from
  keys and values the cores reported, and checked mechanically against that output. **Flycast's keys
  are `reicast_*`**; guessed `flycast_*` keys would have been silently ignored. PPSSPP: JIT, 1x
  (480x272), anisotropic off, frameskip off, X confirms. Flycast: 640x480, threaded rendering,
  auto-skip "some", anisotropic off.
- **Dreamcast BIOS:** `core/emulator.cpp` (v2.7) loads a game with the real BIOS if
  `<system>/dc/dc_boot.bin` exists, and **falls back to HLE automatically** otherwise, so
  `reicast_hle_bios` stays `disabled`.
- On the board, `S46card` created `psp`, `dreamcast` and `_system/bios/dc` on the user's card and
  installed the PPSSPP assets (20 MB, stamp `v1.20.4`). A second run was a no-op.
- **Regression check:** NES on the GLES3 RetroArch at 1080p is fine (`kms`, Mali-G31, GLES 3.1),
  and the launcher came back.
- Package hash files were added for both (Buildroot-generated git tarballs), and Buildroot accepts
  them.
- **Not verified (needs the user and game files):** actually playing PSP and Dreamcast, meaning GLES3
  context creation by the core, speed, audio and controls. No ROMs were downloaded.

**Test plan for tomorrow:** put a PSP `.iso`/`.cso` in `psp\` and a Dreamcast `.chd`/`.cdi` in
`dreamcast\` (or a GDI set in its own subfolder), restart the launcher, and check:
1. both systems appear and list their games; a GDI folder appears as one game;
2. the game starts, and `/tmp/retroarch_verbose.log` shows a GLES3 HW context;
3. speed and audio, by ear first, then measured (the reference's audio-seconds / wall-seconds ratio);
4. the controls: the DS3 analog stick on PSP, Dreamcast triggers;
5. save states through the launcher's slot picker (new for PSX/N64/GBA/PSP/DC).

### PSP first play: works, heavy 3D games are slow

User tests: PSP games start and run on the GPU. Need for Speed Most Wanted and Assassin's Creed
Bloodlines are "slow, not choppy", i.e. below real-time.

**Measuring speed:** the RetroArch wrapper now exports `RETROARCH_LOG_FPS=1` (our patch 0001), which
logs `[Video]: FPS: x/59.94` every 256 frames. PPSSPP always delivers 60 fps of game time (frame
duplication), so FPS/60 is the emulation speed. Loading screens read *above* 60 (74-77 fps: no
audio, so nothing paces them), so only gameplay samples count.

**Assassin's Creed Bloodlines, gameplay, Skip Buffer Effects on:** 18.3 / 19.3 / 19.3 / 20.1 fps,
**avg 19.2 = 32% speed**.

Profile during that gameplay:
- GPU (panfrost fdinfo, with `/sys/.../1800000.gpu/profiling` on only for the measurement):
  fragment 57%, vertex/tiler 20%.
- Threads: Main (RetroArch plus PPSSPP's GL command thread) 76%, EmuThread (the PSP CPU JIT) 38%.
  No core and no engine is at 100%, so CPU and GPU are **taking turns**, not saturated.
- `strace` on Main: in 2 s, **1,030 `PANFROST_SUBMIT`** (~27 GPU jobs per frame) and ~8,000
  BO create/label/madvise ioctls, plus ~3,000 mmap/munmap in 5 s. Heavy per-frame driver overhead
  and frequent flushes, each costing a tile store/reload on this tiler GPU.
- **Mesa `glthread`:** `mesa_glthread=true` was in the process environment, but no glthread driver
  thread appeared, so Mesa did not enable it in this EGL/GBM setup. **Inconclusive** (18.5 fps, noise).
  Reverted.
- Earlier, NFS MW before Skip Buffer Effects: GPU fragment 70.6%.

Context: AC Bloodlines is among the heaviest PSP titles, and on H700 handhelds (same CPU/GPU class)
it is generally considered unplayable. Open levers:
1. **GPU clock.** It is fixed at 432 MHz, because the H616 DT has no GPU OPP table and so there is
   no devfreq. Needs research into safe voltage/frequency pairs (BSP/Armbian) before trying, and
   measurement afterwards (the reference's Mali-400 overclock bought nothing).
2. Why glthread does not engage.
3. Calibrate with lighter PSP games before judging the port.

**LittleBigPlanet (mid-weight 3D).** First reading 51.2 fps (85%). Then a messy A/B, for two
reasons found along the way:
1. ~~Option changes made in RetroArch's menu did not reach `PPSSPP.opt`.~~ **RETRACTED: saving
   works.** A controlled test (change Texture Filtering → Linear, exit with PS+Start, read the file)
   showed the file written seconds later with the new value. The earlier conclusion came from reading
   the file while a game was still running, or after the menu's Restart, which does not save. It
   was never isolated before being logged as a bug. Two things are still true and worth knowing:
   RetroArch writes core options **when the core unloads**, and it **rewrites a per-game file from
   its in-memory values on exit**, which undid the first per-game experiment. So edit option files
   by hand only while no game is running.
2. **Skip Buffer Effects gives LBP a black screen** at launch (twice).
Controlled result, same level: Skip Buffer Effects off gives **37.5 fps without Lazy Texture Caching
and 37.0 with it**, so lazy caching has no effect. Profile: single-thread bound on Main (strace: 0.04 s
GPU wait against 0.66 s futex in 3 s; the rest is CPU in PPSSPP's GPU emulation and Mesa). The CPU is
already at 1416 MHz, which is the top of this speed bin's cpufreq table.

**Decision (user): Auto Frameskip @ 2 "feels playable"** for LBP (unlike AC Bloodlines), and it is now
the PPSSPP default in the image. **Measurement caveat:** with frameskip on, the RetroArch FPS log
counts *rendered* frames, not game time, so it under-reads real speed. Measure speed with frameskip
off, or by audio.

**Dreamcast system hidden with a GDI folder as its only game.** The user put *Sonic Adventure 2*
as a GDI set in `dreamcast/Sonic Adventure 2/`. The files reached the card intact, but the launcher
listed 17 systems, without Dreamcast. The system list had its **own** emptiness check, a loop over
plain files that skipped directories. Last night's fix only covered `count_roms_in_dir()`, not this
second copy of the same idea. It now calls `count_roms_in_dir()`, so there is one definition of
"has a game". Deployed: 18 systems.

### Dreamcast first play, and a GPU overclock that pays off

Sonic Adventure 2 (GDI) boots on Flycast's HLE BIOS ("Did not load BIOS, using reios") and renders
on the Mali-G31, GLES 3.1. **The user timed the in-game clock: 11.8 s of wall time per 10 s of game
time = 85% speed**, with auto frame skip at "more". That is a better method than our FPS log, which
under-reads whenever frames are skipped (the log said 22 fps). The profile showed it GPU-bound.

**GPU OPP table (patch 0050).** No H616/H618 GPU OPPs exist in mainline or in Armbian. Orange Pi's
own BSP (`orange-pi-6.1-sun50iw9`, `sun50i-h616.dtsi`) has 125/250/432 MHz @ 810 mV, 600 @ 960 mV,
800 @ 1080 mV, but its `operating-points-v2` line is **commented out** there. On the Zero3 the
GPU rail is dcdc1 "vdd-gpu-sys" (810–990 mV, shared), which ran at 900 mV. Our table: 432 @ 900 mV
(never below the previous voltage) and 600 @ 960 mV. 800 MHz is out, since 1080 mV is above the
rail's 990 mV. The patch was generated by diffing an edited copy of the build-tree DTS, read before
building, and checked in the compiled DTB (0x19bfcc00 / 0x23c34600 Hz, 0xdbba0 / 0xea600 µV).

Result, ~~same spot~~ (it was NOT the same spot, see the correction below): **10.5–11 s per 10
game-seconds = 91–95%** (from 85%). **Withdrawn**: a locked-clock A/B later measured +2 points. A 60 s watch in play:
600 MHz for 51 of 60 samples (simple_ondemand drops to 432 in light scenes), rail at 960 mV,
**peak GPU 60 °C** (57 before), **no panfrost faults**. Kept. Unlike the reference's Mali-400
overclock (memory-bound, no gain), this workload is really GPU-bound.

Still possible for Dreamcast: alpha sorting "per-strip (fast)" and 320x240 internal resolution,
not tested yet.

**CORRECTION: the FPS-log numbers in this session are not emulation speed.** The user doubted
them, and the data agrees: Sonic Adventure 2 logged 22 fps while the in-game clock gave 85%
(≈51 fps). The log counts frames RetroArch *presents*. Frameskip hides skipped frames, games that are
natively 30 fps (common on PSP) present half as many, and whether PPSSPP's "duplicate frames to
60 Hz" duplicates are counted is unverified. So the "19 fps = 32%", "37 fps = 62%" and "51–53 fps =
85–88%" conversions above are **invalid as speed figures**. They are only comparable run to run
under identical settings. The only trustworthy speed data this session is the user's
stopwatch-on-the-game-clock.

**Audio speed meter (RetroArch patch 0007).** `RETROARCH_LOG_SPEED=1` (now set in the wrapper) logs
every ~5 s:

    [Audio]: Speed: 100.0% (core audio 5.02 s in 5.02 s wall)

This is the core's audio frames ÷ its nominal sample rate ÷ wall time, counted in
`audio_driver_sample()` and `audio_driver_sample_batch()`. Emulated time is what the audio tracks,
so frameskip and duplicated frames do not affect it. Windows with the menu open, or with a core that
produces no audio, read low. Validation on NES (known full speed): 103.1% (startup buffer fill),
then **100.0%, 100.6%**. Generation notes: the file includes no `<stdlib.h>`, so the patch adds it
for `getenv()`; GCC 14 would reject an implicit declaration. The `\n` in the format string was
checked in the generated patch, given this project's backslash history.

**Sonic Adventure 2 with the meter, and a CORRECTION to the GPU-overclock result.**
- Meter over one stretch: 72.5% (68–79%). The user's stopwatch at "the usual spot": 11.8 s per 10
  game-s = 84.7%, while the meter at that same moment read **82.5% (~83% steady)**. The two agree
  within stopwatch precision, **so the meter is validated on Flycast too**. The 72.5% was a heavier
  stretch.
- But 11.8 s is also what the stopwatch gave at **432 MHz** before patch 0050. The "10.5–11 s = 91–95%"
  run was at a different spot. **The claimed 85% → 91–95% overclock gain was a comparison across
  scenes, and is withdrawn.**
- Clean A/B, same spot, GPU clock locked live through devfreq min/max_freq, 30 s each:
  **432 MHz 75.5%, 600 MHz 77.8%**. About +2 points: real but small. Kept, since it only engages
  under load, costs ~3 °C and showed no faults. So SA2 is **not mainly GPU-bound**. The fdinfo
  "fragment 236%" that suggested it was double-counted across GL contexts. Next: a CPU profile
  (Flycast's SH-4 dynarec on the A53).
- Lesson, again: compare runs only at the same spot, preferably A/B within one session.

**Dreamcast is CPU-bound; DSP off.** Profile during SA2 (meter ~80%): thread `Flycast-emu` at
**96.8%** of one core (cpu0 100%), the rendering thread at 25%, the other cores mostly idle. The CPU is
at 1416 MHz, the top of this bin. So it is the SH-4/AICA emulation thread. Flycast's CPU-side knobs
(from core-info): `reicast_enable_dsp` (System) and `reicast_sh4clock` (Emulation Hacks, default
200). Same spot, live toggle: DSP on **79.3%** (78.6–79.5, steady) → DSP off **86.5%**, then
**96.8%** (87.6→101.1, rising). The scene changed during the second run, and the A/B/A confirmation
was skipped because the user judged the game "much better" and chose DSP off. **The sound is better
too**, by the user's ear. That fits: below real time RetroArch stretches the audio to keep up (the
reference's N64 lesson), so a clean ~100% stream beats keeping the DSP's reverb. **Now the default in
Flycast.opt.** Untested: SH4 underclock (it helps some games and breaks others; per-game).

**First real measurement: Assassin's Creed Bloodlines, gameplay, 60 s: avg 99.1%**
(12 windows, 97.4–100.3%), with auto frameskip @ 2 and the GPU OPP table (hottest zone 59 °C).
The FPS log had put this same game at "32%". This title is effectively full speed on this board.

**PSP after the overclock:** the user reports **Assassin's Creed Bloodlines now playable**, with
PPSSPP's defaults auto frameskip @ 2 and the GPU at 600 MHz (texture filtering Linear). Earlier it
was 19 fps / ~32% speed with no frameskip at 432 MHz, the worst title tested. Reported by eye, not
measured. The FPS log cannot measure it with frameskip on, so the stopwatch method is the one to use.

- Build note: every `make` now spends ~1 min in Buildroot's `setlocalversion`, which runs `git
  update-index --refresh` in the BR2_EXTERNAL tree. Since the project became a git repo that tree is
  a git work tree on `/mnt/c` (9P), where every stat is slow. Harmless, just slow.

Tooling note: an inline Python edit of this log died on Windows' cp1252 default encoding (the
`≤` above) **after** `open(p, 'w')` had already truncated the file to 0 bytes. It was restored from
git, since all prior entries were committed. Use the Edit tool for these docs, or pass
`encoding='utf-8'`. Never open-for-write a file you haven't committed.

**Full game names in the launcher (arcade and PSP).** Arcade ROMs are named after the emulator's
driver (`mslug.zip`), and PSP dumps after the release group (`psy-acb.iso`). The launcher now
resolves a display name in this order (`rom_display_name()` in launcher.c):
1. the folder's `gamenames.txt`: the user's own names always win;
2. the arcade core's driver list, `/usr/share/retroopi/names/{fbalpha2012,mame2003plus}.txt`.
   post-build.sh generates it with `scripts/gen-arcade-names.py` from the source of the exact core
   versions being built: 5984 FBA and 4486 MAME titles. Neo Geo catalogue codes ("(NGM-2410)") are
   dropped; variant tags ("(World)", "(bootleg)") are kept;
3. the PSP disc's own `TITLE` from `PSP_GAME/PARAM.SFO`, read from `.iso`, `.cso` (zlib, so the
   launcher now links `-lz`) or `.pbp`. ™/®/© and trailing spaces are dropped because the font has
   no glyph for them. Tested on the four ISOs and on a CSO made from one of them:
   "Assassin's Creed: Bloodlines", "Castlevania The Dracula X Chronicles", "LittleBigPlanet",
   "METAL GEAR SOLID PEACE WALKER";
4. the file name.

BIOS sets (marked `*` in the tables: `neogeo`, `stvbios`, `pgm` …) and `gngeo_data` are hidden from
the lists and from the game counts. Neo Geo now shows 32 games, down from 34. The files stay on the
card, because the cores need the BIOS. post-build.sh fails the build if a table comes out short or
`neogeo` is not marked as BIOS.
- Trap: FBA defines `neogeo` more than once (the BIOS-only board and system variants). The first
  generator let a later non-BIOS entry overwrite the BIOS mark. Now any BIOS definition wins.
- Trap: `PARAM.SFO` in Assassin's Creed sits at LBA 54960 (~107 MB). A test CSO made from the
  first 96 MB therefore "failed". The test was the problem, not the reader.

**Compiler optimisation levels, and -O3 for Flycast and PPSSPP.** No level was ever chosen
deliberately. Buildroot builds everything `-O2` (BR2_OPTIMIZE_2), with `-mcpu=cortex-a53` from the
toolchain wrapper. Dry-running each core's build (`make -n -B` with its real platform and flags)
shows which cores override that:
- PCSX-ReARMed `-Ofast -ffast-math`, ParaLLEl N64 `-Ofast`, gpSP `-O3 -ffast-math`, FUSE `-O3`;
- everything else is `-O2`: FBA 2012, MAME 2003-Plus, the SNES/NES/Genesis/GB cores and others.
- **PPSSPP and Flycast were `-O2` too**, although both are CMake Release builds. Buildroot's
  toolchain file sets `CMAKE_{C,CXX}_FLAGS_RELEASE` to just `-DNDEBUG`, which drops upstream's
  `-O3`. It sets them only `if(NOT DEFINED ...)`, so both packages now pass
  `-DCMAKE_{C,CXX}_FLAGS_RELEASE="-O3 -DNDEBUG"`. The Release flags follow `CMAKE_CXX_FLAGS` on
  the compile line, so `-O3` wins, as the generated `flags.make` confirms.

Measured, Sonic Adventure 2, same spot, 60 s each, audio speed meter: **-O3 86.9%** (80.5–101.8)
vs **-O2 85.3%** (78.2–93.6). That is +1.6 points against a 10–20 point swing from the scene alone,
so **within noise**. The planned third run (-O3 again) was not done. Expected: most of the
emulation thread runs in code Flycast's SH-4 recompiler generates at runtime, which compiler
flags do not touch. Kept, because it costs nothing and both builds ran cleanly, but **not
claimed as a speed-up**. PSP at -O3 is unmeasured.

- Traps: `make <pkg>-reconfigure` failed once with "No rule to make target" while a second
  Buildroot `make printvars` loop was running in the same output dir. It worked when rerun alone.
  Also, a reconfigured CMake core lands in target/ **unstripped** (PPSSPP 35 MB vs 18 MB) until
  target-finalize strips it. Strip it before a `board.sh push`.
- **Dreamcast is single-threaded by design, not by a missing option.** Flycast already renders on
  its own thread (`reicast_threaded_rendering`). The SH-4, the sound chip and the scheduler share
  one thread, because the rest of the hardware is timed against the SH-4, and no Dreamcast
  emulator splits it. Flycast's `pvr.MaxThreads` (option.cpp, default 3) looked promising but only
  caps the OpenMP threads of **xBRZ texture upscaling** (TexCache.cpp). We build with
  `USE_OPENMP=OFF`, and the libretro port gives it no option key, so it is inert here.

**Controls help screen.** A tap of PS in the launcher shows a full-screen picture of the pad with
every binding: the launcher's own in white, the in-game PS + button hotkeys in amber
(retroarch.cfg, plus volumed's PS + Up/Down volume). `scripts/gen-help-image.py` draws it with
Pillow (Windows' Python; WSL's has none). The 1920×1080 PNG is committed in the rootfs overlay,
and the launcher scales it once to the screen. **When a binding changes, update the script and
regenerate the PNG.**
- PS is a modifier, so a PS press that is part of a combo must not open the help. The launcher
  tracks it: a button pressed while PS is held marks the hold as a combo and is ignored until
  released, for both presses and key repeat. That also fixes a bug: the list cursor used to move
  along with PS + Up/Down volume changes.
- The screen closes on the **release** of a button. Closing on a press would leave the release to
  the main loop, and a PS release there opens the help again.
- A "PS HELP" hint in the systems footer is the only pointer to it. Tested on the board: shown and
  closed as expected.
- For the record: **PS + R1 saves and PS + L1 loads**, following retroarch.cfg.

**exFAT for the ROM partition.** Asked for after a question about supported filesystems. As
built, only FAT32 was possible: `CONFIG_EXFAT_FS` and both NTFS drivers were off, and fstab
mounted p2 as `vfat`. Now:
- Kernel: `CONFIG_EXFAT_FS=y`, `EXFAT_DEFAULT_IOCHARSET="utf8"`. `CONFIG_MSDOS_FS` is pinned
  off, because an `auto` mount tries every filesystem the kernel lists, and msdos would accept a
  FAT32 card and show only 8.3 names.
- fstab: p2 is `auto` with `rw,noatime,umask=0022`. The old `shortname=mixed,utf8` are
  vfat-only options that would make an exFAT mount FAIL, and the launcher would show no games.
  They were also redundant: `FAT_DEFAULT_UTF8=y`, and mixed is vfat's default. The resulting vfat
  mount options are byte-identical to before. post-build.sh fails the build on a non-`auto` type
  or a vfat-only option; the guard was tested against the real fstab and two broken ones.
- NTFS deliberately not added: Linux refuses, or mounts read-only, an NTFS volume Windows did not
  cleanly eject.

Verified on the board. FAT32 and exFAT test images (host `mkfs.vfat`, and a Buildroot-built
`host-exfatprogs`, since WSL has none and apt needs sudo) mounted with the fstab options. Long,
mixed-case and non-ASCII names ("Pokémon Café.gba") survived a remount byte for byte, and lookup
is case-insensitive on both, as in Windows.
**Surprise: the user's own card was already exFAT**, reformatted in Windows. It mounted as exfat on
the first boot of the new kernel, and the launcher found 20 systems. The old kernel could not have
mounted it at all.
- Trap: busybox `ls` prints non-ASCII bytes as `?`. Check names with `od -c`, not by eye.

**Dreamcast "stopped working": `flycast_libretro.so` on the board was an empty file** (md5
`d41d8cd98f00b204e9800998ecf8427e`, the md5 of zero bytes). RetroArch could not load the core, so
every Dreamcast game failed. The `.O2`/`.O3` A/B copies next to it were intact, and the board's
boot log showed ext4 journal recovery, i.e. an unclean power-off. Restored from `.O3` with
`cp` + `sync` + `mv` (md5 and core-info verified). A full comparison of usr/ lib/ bin/ sbin/ against
target/ found no other damaged file.
- Cause, honestly: **not proven.** The A/B swaps replaced the core with a bare `cp` onto the live
  file (truncate, then write) and no sync. Truncate-then-rewrite plus an unclean shutdown is the
  textbook way to get an empty file on ext4. But later pushes ran `sync`, which should have
  flushed it, so the timing does not fit neatly.
- Fix regardless: `board.sh push` now writes `<path>.new`, syncs, and renames over the target. A
  rename is atomic, so a power cut leaves the old file or the new one, never a truncated one. Any
  hand-written swap script on the board should do the same.
- Lesson: when one system stops working after a board-side file change, md5 its core first.
  `md5sum` of an empty file is instantly recognisable.

**Performance deep dive ("an RP2 with 4× A7 and a Mali-400 runs PSP/Dreamcast well, why do we
struggle?").** Tools: `perf` built dev-only (BR2_PACKAGE_LINUX_TOOLS_PERF in output/.config,
**not** the defconfig). Linux 6.18's perf does not build with NO_SLANG: the
`hist_entry__tui_annotate` stubs in util/hist.h carry an extra `u64 al_addr` argument, fixed by
hand in the build tree. The tool is copied to /tmp on the board. The measurement script reports,
per window: audio-meter speed, displayed FPS, per-thread CPU, cpufreq residency, temps, GPU
engine busy time (panfrost fdinfo `drm-engine-*`, after writing 1 to `.../panfrost/*/profiling`),
and a perf profile of the busiest thread. Stripped .so addresses are resolved with the build
tree's copies: Mesa's `libgallium` in buildroot-build is unstripped with identical .text, and
Flycast relinks without `-s` from its `link.txt`.

Static audit, all clean:
- CPU pinned at 1416 MHz, 100% residency in gameplay. The cpu-thermal 60/70 °C passive trips have
  **no cooling device bound**, so they never throttle; only the 110 °C critical trip acts.
- DRAM LPDDR4 at 792 MHz. JITs compiled in (Flycast rec_arm64, PPSSPP MIPS/ARM64). Quiet dmesg.
  No run-ahead or rewind.

**Dreamcast, Sonic Adventure 2:** `Flycast-emu` 98.7% of a core, ~75% of it in **JIT-generated SH-4
code**. Flycast's code cache `SH4_TCB` is an 11 MB array inside .text, so addr2line labels it
`rdv_BlockCheckFail`, the preceding symbol; `nm` shows `SH4_TCB` at 0x837000. **~23% of the
thread is one busy-wait loop in the game**, polling game-RAM counters (0x8c29c68c/…c7d4). Flycast
has no idle-loop skip. The SH-4 underclock (`reicast_sh4clock`) is the lever, but it is applied
at **block compile time** (decoder.cpp scales `guest_cycles`), so a live change only affects new
blocks; it must be tested from a fresh start. Live 160 MHz gave 90.4% vs 85.9%: inconclusive.
GPU fragment ~126% busy, from Flycast's own 640×480 scene rendering.
**720p output does not help:** 80.8% speed and 121% fragment-busy, i.e. the scaling pass was
never the cost.

**GPU pipeline:** Flycast renders 640×480 into its own FBO (postProcessor), copies it into
RetroArch's FBO, and RetroArch draws one nearest-neighbour quad (video_smooth defaults to false
here) onto a 1920×1080 GBM buffer that KMS flips. The DE33 does no scaling; RetroArch KMS never
uses plane scalers.

**PSP, Assassin's Creed Bloodlines (1080p):** 99% speed, 37.8 fps displayed (auto frameskip).
`EmuThread` only ~33%. The limit is PPSSPP's render thread (`Main`, 76%), **84% of it inside Mesa
on the CPU**: ~50% `pan_access_tiled_image_generic(_aligned)`, CPU (de)tiling of texture uploads,
and ~16% `u_vbuf_get_minmax_index_mapped`, scanning index buffers for min/max per draw. The
minmax cache misses because PPSSPP streams its index data. GPU fragment only 62% busy.
`PAN_MESA_DEBUG=linear` A/B: tiling gone, but `memcpy` takes ~20% (the upload volume itself),
the kernel 17%, GPU fragment 76%, and displayed fps 34.3 vs 37.8. **No gain; not adopted.**
Panfrost's own auto-linear conversion (LAYOUT_CONVERT_THRESHOLD 8) does not trigger for PPSSPP.

Conclusions: nothing in our setup is misconfigured. Dreamcast is bound by the SH-4 JIT, a quarter
of it a game wait loop. PSP is bound by **Mesa panfrost's CPU overhead** for PPSSPP's per-frame
texture uploads and streamed indices, not by the GPU. The RP2 runs ARM's proprietary Android
driver and old GLES2 emulator builds, a different workload. Its claimed results are
**unmeasured** here: a like-for-like stopwatch test on the same game was proposed.
Test hook left on the board: the RetroArch wrapper sources `/tmp/ra.env` if present (gone after a
reboot).

**PSP: the "something really basic" (user, on Burnout Legends: clean on a Retroid Pocket 2,
noticeably worse here).** Burnout at 1080p, measured: speed **88.4%**, **32 fps displayed with
dips to 9.8**, PPSSPP's render thread (`Main`) at 86% of a core, but the **GPU only 33% busy**.
Frame-pointer call chains (`perf record -g --call-graph=fp`) work through Mesa. Resolved:

    PPSSPP GLQueueRunner::PerformCopy → glCopyImageSubData → st_CopyImageSubData
      → util_resource_copy_region → panfrost_ptr_map → pan_load_tiled_image   (~60%)
    PPSSPP PerformRenderPass → glDrawElements(BaseVertex) → panfrost_draw_vbo
      → panfrost_get_index_buffer_bounded → u_vbuf_get_minmax_index_mapped        (~13-37%)

1. **Panfrost has no GPU image copy:** `pan_resource.c:2556`
   `pctx->resource_copy_region = util_resource_copy_region`. Every same-format
   glCopyImageSubData is a CPU copy: sync the GPU, map, untile, retile. PPSSPP uses image copies
   for framebuffer effects whenever a `*_copy_image` extension exists (`framebufferCopySupported`),
   and otherwise falls back to glBlitFramebuffer, which panfrost runs on the GPU. Proven first
   with `MESA_EXTENSION_OVERRIDE="-GL_OES_copy_image -GL_EXT_copy_image"`: the tiling vanished, the
   GPU went to 87% busy, the dips were gone (min 27.3 fps), speed 90.6%.
   → **0002-gl-no-copy-image-on-mesa-panfrost.patch**: `framebufferCopySupported = false` when
   `gpuVendor == GPU_VENDOR_MESA` and the model contains "Panfrost".
2. **Index min/max scan per draw:** panfrost needs index bounds and, without them, scans the index
   buffer on the CPU, reading GPU memory. Its minmax cache misses because PPSSPP streams indices.
   PPSSPP calls plain glDrawElements although DecodeIndsAndGetData already knows the vertex count
   (its `maxIndex` output is `numDecodedVerts_`, a COUNT).
   → **0001-gl-pass-index-range-to-the-driver.patch**: the draw command carries `maxIndex`; with a
   known range and GLES3, glDrawRangeElements(0, count-1). Hardware-transform path only; the
   software path can expand vertices and keeps glDrawElements. Mesa then sets
   `index_bounds_valid` and skips the scan.

Result, same stretch of Burnout: **speed 100.2%, 39.6 fps displayed (min 35.9)**. The render
thread fell from 86% to 55%, the PSP EmuThread is now the busiest at 63%, the GPU fragment engine
is at 106% (doing the work), 0 audio underruns. The user: "runs much smoother now". Both patches
are generated by editing pristine copies and `diff -ruN`, and apply with -p1 to v1.20.4.

Also measured in passing:
- `PAN_MESA_DEBUG=linear` (no tiling at all): no gain. The tiling cost became memcpy, and GPU
  sampling got dearer.
- **Vsync: RetroArch KMS with `video_vsync = false` DROPS every frame that finishes while a page
  flip is pending** (drm_ctx.c `gfx_ctx_drm_swap_buffers`: "nonblocking mode, so just drop the
  frame"). It was turned off on the reference board for audio reasons and never re-validated here.
  All the PSP runs above had vsync ON on the board (test change, not in the repo yet), with 0
  underruns. Making it the default needs a Dreamcast and an N64 check first.

**vsync ON by default; the SH-4 underclock is not a lever for Sonic.**
- vsync on, measured on the board in every system tested, with HDMI audio, latency 128 and the
  default max swapchain of 3. Burnout: 0 underruns. Assassin's Creed: 0. Sonic Adventure 2: 84.3%
  (vs 85.9% off), 0 underruns. Mario Kart 64: 100.1%, a locked 60.0 fps, 0 underruns. The
  reference's audio failure with vsync on (A33 codec) does not reproduce on this board, and vsync
  off drops frames in RetroArch KMS (see above). retroarch.cfg now ships `video_vsync = "true"`.
- `reicast_sh4clock = "160"`, written into Flycast.opt so it applies from the first block: Sonic
  83.9% / 17.1 fps vs 84.3% / 22.3 fps at 200. **No gain**; the wait loop shrinks with the clock.
  The earlier "+5.5" came from a live change plus scene noise. Restored to 200 (the board keeps
  `Flycast.opt.orig`).
- Two observations, left open:
  - RetroArch did not persist the in-menu SH-4 change to Flycast.opt on exit.
  - Dreamcast's limit stays the SH-4 JIT on one A53 core; Flycast's render thread is fine (~40%)
    and does not hit PPSSPP's panfrost CPU paths.

**CPU: thermal protection, and 1512 MHz (patch 0051).** The user plans a fan and asked to raise
the CPU clock for Dreamcast, which is bound by one A53 core.
- Facts first. The OPP table (sun50i-h616-cpu-opp.dtsi) is filtered by efuse speed grade
  (`opp-supported-hw`, voltages from `opp-microvolt-speedN`). This chip accepted
  720/936/1104/1320/1416 and not 1512, so its grade is 0 or 2, both capped at 1416 MHz @ 1.10 V.
  vdd-cpu (AXP313 dcdc2) is limited to 1.10 V in the Zero3 dts. The cpu-thermal zone's 60/70 °C
  passive trips had **no cooling-maps**, so the CPU had no throttling below 110 °C critical.
- 0051 (board dts): trips raised to 80/90 °C and bound to cpu0-3 (map0 on the 80 °C trip,
  verified `cdev0` on the board); 1512 MHz allowed for grades 0/2 at 1.10 V (supported-hw 0x2f).
  An overclock for this grade, at an unchanged voltage. The DTB was decompiled and checked before
  deploying.
- Stability: tools-free, self-checking stress test (4 workers md5+sha256 a 48 MB random file in
  tmpfs; references taken at 1416 MHz). 10 min, 1442 rounds (~138 GB), **0 mismatches**, 100%
  residency at 1512, CPU peak 60 °C without a fan, throttle state 0.
- Result: Sonic Adventure 2 **84.3% → 89.9%** (1512 MHz 100% of the window, 0 underruns): the
  +6.6% matches the +6.8% clock, as the profile predicted. Higher clocks need more than 1.10 V,
  i.e. raising the board's regulator limit. That was deliberately not done without the user's
  explicit go-ahead and a fan.

**Shaders: smooth pixel-art upscaling for Game Boy / GBC.** The user asked for "rounded edges"
(anisotropic filtering is irrelevant to 2D). Findings, measured with the GPU fdinfo counters:
- **Shaders were never applied at game start.** RetroArch's DEFAULT_SHADER_ENABLE is false on
  non-console builds, a menu "Load" only enables shaders for the session, and config_save_on_exit
  is off. So the user's saved presets were written but never loaded. Now
  `video_shader_enable = "true"`.
- libretro glsl-shaders (commit f8e23ff) mostly targets desktop GLSL. On GLES 3.1 we hit:
  implicit int→float (reverse-aa post3x pass0/pass1), a fragment output declared before the
  precision statement, and non-constant global initialisers (xbr-lv2). Fixed in our copies. The
  presets also reference other folders (`../anti-aliasing`, `../blurs`, …), so a partial copy breaks.
- **The Mali-G31 MP2 budget is small.** Game Boy with a do-nothing preset: 100.5%, GPU 30%.
  ScaleFX (5 passes, 3×): 44.5% (float framebuffers made no difference). xBRZ 3×: 36.7%. xBR-lv2
  computed at full screen resolution is hopeless; at 2× and highp: 65.1%.
- **16-bit floats double the shader throughput** (Bifrost fp16): xBR-lv2 at 2× with `precision
  mediump` gives **100.5%, a locked 60 fps** (GPU 172%). xBRZ 2× mediump: 88-99%, scene-dependent
  (rejected). HQ2x + bilinear: 100.5%, GPU 69%.
- Shipped: /usr/share/retroopi/shaders/ (xbr-lv2-mp.glsl with the GLES fixes and mediump,
  stock.glsl, xbr-lv2-2x-mp.glslp, README with the measurements). Folder presets
  config/Gambatte/gb.glslp and gbc.glslp `#reference` it; GBC also measured 100.5%, 60 fps.
  post-build checks that every referenced preset exists (tested against a complete target and
  one with the preset removed).
- The full collection (39 MB) went on the ROM card for testing (_system/shaders). The rootfs is
  only 1 GB and was 92% full.
- Traps:
  - busybox tar has no -z.
  - `ssh` inside `while read` swallows the loop's stdin (use `ssh -n`).
  - core-info cannot dlopen C++ cores that need libstdc++ (Gambatte); RetroArch can.
