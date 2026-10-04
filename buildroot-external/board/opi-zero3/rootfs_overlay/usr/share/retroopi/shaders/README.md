# Shaders shipped in the image

From [libretro/glsl-shaders](https://github.com/libretro/glsl-shaders), commit
`f8e23ff880668f0f0e837a05a316534d82a7f31b`, and adapted for the Mali-G31
(Mesa panfrost, OpenGL ES 3.1). Each file keeps its original licence header.

| File | Source | Changes |
|---|---|---|
| `xbr-lv2-mp.glsl` | `xbr/shaders/xbr-lv2.glsl` (Hyllian, MIT) | **GLSL ES fixes:** `out mediump vec4 FragColor` (the output is declared before the precision statement, and ES fragment shaders have no default float precision); the `delta`, `delta_l` and `delta_u` globals made `const` with literal initialisers (ES needs constant global initialisers). **16-bit floats:** `precision mediump float` instead of highp. |
| `stock.glsl` | `stock.glsl` | unchanged |
| `xbr-lv2-2x-mp.glslp` | ours | xBR at 2× the source, then a bilinear stretch to the screen |
| `hqx/hqx-pass1.glsl`, `hqx/hqx-pass2.glsl`, `hqx/hq2x.png` | `hqx/shader-files/`, `hqx/resources/` (Maxim Stepin, Cameron Zemek, Jules Blok; LGPL-2.1+) | unchanged; they compile on GLES as shipped |
| `hq2x-smooth.glslp` | `hqx/hq2x.glslp` | HQ2x, then a bilinear stretch to the screen (third pass) |
| `hqx/hq2x-halphon-mp.glsl`, `hq2x-halphon-mp.glslp` | `hqx/shader-files/hq2x-halphon.glsl`, `hqx/hq2x-halphon.glslp` (Lior Halphon, MIT) | `precision mediump float` (16-bit); one pass at 2×, then a bilinear stretch |

Defaults (RetroArch folder presets in `/root/.config/retroarch/config/`):

| ROM folder | Preset | Measured |
|---|---|---|
| `gb`, `gbc` (Gambatte) | `xbr-lv2-2x-mp` | 100.5%, 60 fps, GPU 172% |
| `atari800` (Atari800; 800 and 5200) | `hq2x-smooth` | 100.0%, 59.9 fps, GPU 123%; ~336×240 source, no 16-bit needed |
| `gba` (gpSP) | `hq2x-halphon-mp` (the user's choice) | 99.0%, 59.0 fps, GPU saturated: about one dropped frame per second, accepted for the look. highp was 89.7%, 54 fps. A 16-bit final stretch pass gained nothing (99.0%), so the stock pass stays. For comparison: hq2x-smooth 100.5%, 60 fps, GPU 72%; xBR-lv2 2× mediump too heavy, 72.6%, GPU 199% (240×160 is 2.7× the Game Boy's pixels) |

## Why these settings

Measured on Game Boy (Gambatte, 160×144 source, 1080p output). The Mali-G31 MP2
is a small GPU, and the filter's cost is mostly a matter of how many pixels it
computes:

| Preset | Speed | GPU fragment busy |
|---|---|---|
| none (nearest) | 100.5% | 30% |
| ScaleFX (5 passes, 3×) | 44.5% | 198% (saturated) |
| xBRZ at 3× | 36.7% | 199% |
| xBR-lv2 at 2×, highp | 65.1% | 198% |
| **xBR-lv2 at 2×, mediump** | **100.5%, 60 fps** | 172% |
| xBRZ at 2×, mediump | 88–99%, depends on the scene | ~180% |
| HQ2x + bilinear | 100.5% | 69% |

- 16-bit floats roughly double the shader throughput on this GPU (Bifrost runs
  fp16 at twice the fp32 rate). For colours and Game Boy-sized texture
  coordinates the precision is ample.
- The filter runs at 2× (320×288). A plain bilinear pass does the rest of the
  stretch, so the expensive edge logic is never run at screen resolution.

A folder preset applies only to its ROM folder, so `gb` and `gbc` (one core) can
differ.
