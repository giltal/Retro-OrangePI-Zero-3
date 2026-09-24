# Context — RetroOPI_Z3

> Port of **RetroBPI_M2M** (Banana Pi M2 Magic, Allwinner A33, DSI panel) to the
> **Orange Pi Zero3** with HDMI output.
> Reference project: `C:\BananaPi_Projects\RetroBPI_M2M`
> Started: 2026-09-23

---

## Hardware — Orange Pi Zero3 (2 GB)

| Item | Detail |
|---|---|
| SoC | Allwinner **H618**: 4× Cortex-A53, up to 1.5 GHz depending on speed bin. H616 family, `sun50i` |
| GPU | **Mali-G31 MP2** (Bifrost). Mesa **panfrost**, GLES 3.1 |
| RAM | 2 GB LPDDR4 |
| Display | **micro-HDMI** (DW-HDMI 2.0 behind the DE33 mixer and TCON-TV). No DSI |
| PMIC | AXP313a: dcdc1 → GPU (`mali-supply`), aldo1 → HDMI PHY (`hvcc-supply`) |
| Network | Gigabit Ethernet (Motorcomm YT8531 PHY). **Dev link.** |
| Wireless | Unisoc **UWE5622** Wi-Fi + BT, needs an out-of-tree driver. **Not enabled yet.** |
| USB | 1× USB-A host, USB-C (power + OTG), plus 2× USB 2.0 on the 13-pin header |
| Audio | HDMI audio (H616 audio hub), and analog LINEOUT on the 13-pin header |
| Storage | microSD only (SPI NOR on some SKUs, unused) |
| Power | 5 V via USB-C |
| Debug UART | 3-pin header, UART0 = `ttyS0`, 115200 8N1, 3.3 V |

## What differs from the reference, and what we did about it

| Area | RetroBPI_M2M (A33) | RetroOPI_Z3 (H618) |
|---|---|---|
| Arch | ARMv7 hard-float | **AArch64**. Every core's platform detection re-audited |
| Kernel | mainline 6.18.8 + panel patches | mainline **6.18.53** + **49 Armbian patches** for HDMI/audio/GPU |
| Display | DSI 800×480, mounted 180° | **HDMI 1280×720**, no rotation (RetroArch patch 0007 dropped) |
| GPU | Mali-400 / lima, GLES 2.0 | **Mali-G31 / panfrost, GLES 3.1**. Mesa needs LLVM for it |
| Mode choice | panel's only mode | launcher **picks the mode matching SCREEN_W×H**. RetroArch uses `video_fullscreen_x/y` |
| Audio | A33 codec, analog 'Headphone' volume | HDMI via audio hub. **softvol 'Master'** in `/etc/asound.conf`, written by S11alsa |
| Boot | U-Boot SPL | U-Boot SPL + **TF-A BL31** (`lts-v2.12.9`) |
| Network | USB-Ethernet dongle | **onboard Ethernet** |
| Bluetooth | AP6212 (BCM43438) on UART | **USB dongle** for now (btusb as a module + Realtek firmware) |
| Power | AXP223 900 mA VBUS cap, `S05powercap` | not needed. `S05perf` keeps only the `performance` governor |
| Power button | axp20x-pek → volumed → clean poweroff | the Zero3 has none. volumed's handler stays and is harmless |

## Display pipeline

```
DE33 mixer0 ──> TCON-TV0 ──> DW-HDMI (sun8i_dw_hdmi, H616 PHY)
  (sun8i-mixer, DE33 planes)          │
                                       └─ HDMI audio: AHUB I2S (sound/soc/sunxi_v2)
Mali-G31 (panfrost) — render only; Mesa kmsro scans out via sun4i-drm
```

The DRM nodes are panfrost (render-only, no connectors) and sun4i-drm (the display). Probe order
decides which one is `card0`. The launcher (`drm_open_kms()`) and RetroArch both pick the node that
has connectors, a lesson from the reference, where lima did the same.

## Plan

1. **Bring-up image** (this build): boots to the launcher on HDMI 720p, games through RetroArch
   `gl` on panfrost, HDMI audio, DS3 over USB or a BT dongle, SSH over Ethernet.
2. **Measure, don't assume.** Carry over the reference's methods: vblank-counted fps, the
   audio-seconds against wall-seconds ratio, xrun counting. Check every setting inherited from the A33:
   vsync off (does DE33 async-flip?), `video_threaded`, the N64 screensize, audio latency.
3. **Use the GPU.** *Done so far:* PSP (PPSSPP v1.20.4) and Dreamcast (Flycast v2.7) on GLES3,
   N64 at 640x480, 1080p output. Still open: mupen64plus-next against parallel-n64, hardware PSX
   renderers, shaders, PicoDrive for Sega CD / 32X.
4. **1080p.** Set `SCREEN_W/H=1920/1080` and `video_fullscreen_x/y`. Launcher layout scales by
   `UI_SCALE`. Measure the launcher's CPU blit cost at 2.25× the pixels.
5. **Onboard Wi-Fi/BT** via Armbian's uwe5622 extension, so no dongle is needed.
6. **Boot time** pass, as on the reference (`/run/boottiming` is already instrumented by rcS).
