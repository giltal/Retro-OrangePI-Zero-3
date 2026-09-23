/*
 * gfx_accel.h — 2D acceleration hooks.
 *
 * On the Lyra (RK3506B) these were backed by the Rockchip RGA2 engine.
 * The A33 has no RGA; see gfx_accel.c for what happens here instead.
 * The API is kept identical so launcher.c stays byte-for-byte the proven
 * Lyra version apart from panel geometry.
 */
#ifndef GFX_ACCEL_H
#define GFX_ACCEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  rga_init(void);
void rga_cleanup(void);
int  rga_is_available(void);

/* Hardware-accelerated rectangle fill (XRGB8888 framebuffer) */
int rga_fill_rect(void *vaddr, int fb_w, int fb_h,
                  int x, int y, int w, int h, uint32_t color);

/* Hardware-accelerated screen clear */
int rga_clear(void *vaddr, int fb_w, int fb_h, uint32_t color);

/* Hardware-accelerated copy/scale (XRGB8888) */
int rga_copy(void *src_vaddr, int src_w, int src_h,
             void *dst_vaddr, int dst_w, int dst_h);

/* Hardware-accelerated alpha blend (ARGB8888 fg onto XRGB8888 bg) */
int rga_blend(void *fg_vaddr, int fg_w, int fg_h,
              void *bg_vaddr, int bg_w, int bg_h);

#ifdef __cplusplus
}
#endif

#endif /* GFX_ACCEL_H */
