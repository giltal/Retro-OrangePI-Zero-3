# Shaders shipped in the image

From [libretro/glsl-shaders](https://github.com/libretro/glsl-shaders), commit
`f8e23ff880668f0f0e837a05a316534d82a7f31b`, and adapted for the Mali-G31
(Mesa panfrost, OpenGL ES 3.1). Each file keeps its original licence header.

| File | Source | Changes |
|---|---|---|
| `xbr-lv2-mp.glsl` | `xbr/shaders/xbr-lv2.glsl` (Hyllian, MIT) | **GLSL ES fixes:** `out mediump vec4 FragColor` (the output is declared before the precision statement, and ES fragment shaders have no default float precision); the `delta`, `delta_l` and `delta_u` globals made `const` with literal initialisers (ES needs constant global initialisers). **16-bit floats:** `precision mediump float` instead of highp. |
| `stock.glsl` | `stock.glsl` | unchanged |
| `xbr-lv2-2x-mp.glslp` | ours | xBR at 2× the source, then a bilinear stretch to the screen |

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

`config/Gambatte/gb.glslp` (a RetroArch per-folder preset) applies this preset
to the `gb` ROM folder only.
