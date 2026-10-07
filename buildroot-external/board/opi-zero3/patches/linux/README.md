# Kernel patches — Linux 6.18.53

Mainline 6.18 has **no HDMI for the H616/H618**. The DE33 mixer driver is
there, but the H616 HDMI PHY variant, the TCON-TV variant and every display
node in `sun50i-h616.dtsi` are missing. An attempt to upstream the DT half was
NACKed on linux-sunxi in 2026-09 in favour of Jernej Škrabec's
`sun4i-drm-refactor` series. That series is not merged yet.

Armbian carries that refactor, and has shipped it on the Orange Pi Zero3.
Everything here is vendored **unmodified** from:

    https://github.com/armbian/build
    commit 2843c11a74d3a2afae44b362afdfa2409e8ea657  (2026-09-22,
    "sunxi-6.18: rewrite patches against 6.18.53")
    patch/kernel/archive/sunxi-6.18/

The files are renumbered 0001..0049 in Armbian's `series.conf` order, because
Buildroot applies `BR2_GLOBAL_PATCH_DIR` patches in glob order. All 49 were
verified to apply cleanly, in that order, to a pristine linux-6.18.53.

| Ours | Armbian source | What for |
|---|---|---|
| 0001–0042 | `patches.drm/0001–0042` | DE33 refactor, H616 TCON-TV, H616 HDMI PHY, display pipeline DT, `&hdmi` enabled on the Zero3 |
| 0043 | `patches.armbian/0701-…-enable-sound` | H616 AHUB audio driver (`sound/soc/sunxi_v2`), which carries **HDMI audio** |
| 0044 | `patches.armbian/0702-…-add-digital-audio-node` | HDMI audio clocking fix. Without it the PCM runs but no sound comes out |
| 0045 | `…-increase-bl31-reserved-memory` | 512K reserved for BL31, which TF-A lts-v2.12 needs |
| 0046 | `…-zero2w-zero3-cpu-dvfs.dtsi` | CPU OPPs and regulator, so cpufreq works |
| 0047 | `…-orangepi-zero3-enable-gpu-mali` | `&gpu { status = "okay"; }` for panfrost |
| 0048 | `drv-nvmem-sunxi-add-h616-support` | SID/efuse, used for the CPU speed bin and thermal calibration |
| 0049 | `drv-soc-sunxi-sram-add-h616-sram-c1` | SRAM C1 claim for the video engine |

Our own, on top of Armbian's set:

| Ours | What for |
|---|---|
| 0050 | **GPU OPP table**: 432 MHz @ 900 mV and 600 MHz @ 960 mV, from Orange Pi's BSP (`linux-orangepi`, `orange-pi-6.1-sun50iw9`, `sun50i-h616.dtsi`). Without it the Mali-G31 ran fixed at 432 MHz. 800 MHz @ 1080 mV is left out, because it exceeds the board's 990 mV dcdc1 limit. Measured with the audio speed meter, same spot in Sonic Adventure 2, clock locked A/B: 432 MHz 75.5%, 600 MHz 77.8%. A small but real gain; the game is mostly CPU-bound. (An earlier "85% → 91–95%" claim compared stopwatch runs at different spots and is withdrawn.) In play it reaches 600 MHz under load, the rail sits at 960 mV, peak GPU is 60 °C, no panfrost faults. |
| 0051 | **CPU cooling + 1512 MHz.** The H616 dtsi's cpu-thermal trips (60/70 °C passive) had **no cooling device bound**, so nothing ever throttled below the 110 °C critical trip. Now the CPUs are bound to the first passive trip, raised to 80 °C (second trip 90 °C), so normal play is never throttled. The 1512 MHz OPP, offered upstream only to speed grades 1/3/5, is enabled for grades **0 and 2** (this board's chip, whose table stopped at 1416 MHz) at **1.10 V**: the voltage 1416 MHz already uses, the vdd-cpu (dcdc2) maximum in the dts, and what grades 1/3 use for 1512. **This is an overclock for these grades; no voltage is raised.** Verified: 10-minute all-core self-checking stress test (md5 + sha256 of 48 MB against reference hashes taken at 1416 MHz), ~138 GB hashed, 0 mismatches, 100% at 1512 MHz, CPU peak 60 °C without a fan. Sonic Adventure 2: 84.3% → 89.9% (scales 1:1 with the clock). |
| 0052 | **exFAT: plug the directory readahead.** `exfat_dir_readahead()` reads ahead a whole directory cluster with one `sb_breadahead()` per 512-byte sector, unplugged, so the card saw 256 separate requests per 128 KB cluster: **~90 ms the first time any folder was touched**, however few entries it has. Listing the ROM card's 29-folder root cold took 2.48 s. The loop is now plugged, as `exfat_allocate_bitmap()` already does for the bitmap, so the requests merge. The launcher's boot-time scan of its ROM folders went from 2,583 ms to 633 ms (and to ~235 ms with a launcher fix on top). |

Deliberately **not** taken:

- `fixes-6.18/0014` fixes an out-of-tree megous patch we don't carry, and does not apply.
- `drv-thermal-sun8i-guard-against-null-caldata` is defensive only. Its A523 hunk does not apply.
- `drv-drm-sun4i-hdmi-add-audio-support` is for the A10/A20-era HDMI block, not the H616.
- `arm64-dts-…-zero2-zero3-add-wifi` and the `uwe5622` extension: the onboard Wi-Fi/BT is an Unisoc UWE5622, which needs an out-of-tree driver. Deferred.

To move to a newer 6.18.y: bump the kernel version in the defconfig and the
hashes, fetch Armbian's matching commit, and re-run the apply test.
