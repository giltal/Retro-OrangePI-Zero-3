/*
 * gfx_accel.c — no-op 2D acceleration backend (Allwinner A33 / H618).
 *
 * The Lyra port used Rockchip's RGA2 engine (/dev/rga, librga) for fills and
 * clears. That hardware does not exist on the A33, and its Allwinner analogue
 * (G2D) is a different API entirely. Rather than fork launcher.c, every entry
 * point here reports "unavailable" so the launcher takes the CPU paths it
 * already had as fallbacks — those were always present and always exercised
 * when RGA2 init failed on the Lyra, so this is a well-tested code path, not
 * a new one.
 *
 * If fills ever show up in a profile, the options are, in order of appeal:
 *   1. leave it alone — these are solid-colour fills on an 800x480 surface
 *   2. GPU: the A33 has a Mali-400 MP2 and mainline has lima
 *   3. Allwinner G2D, which would mean writing a real backend here
 */

#include "gfx_accel.h"

int rga_init(void)
{
	return -1;	/* no 2D engine; caller falls back to CPU */
}

void rga_cleanup(void)
{
}

int rga_is_available(void)
{
	return 0;
}

int rga_fill_rect(void *vaddr, int fb_w, int fb_h,
		  int x, int y, int w, int h, uint32_t color)
{
	(void)vaddr; (void)fb_w; (void)fb_h;
	(void)x; (void)y; (void)w; (void)h; (void)color;
	return -1;
}

int rga_clear(void *vaddr, int fb_w, int fb_h, uint32_t color)
{
	(void)vaddr; (void)fb_w; (void)fb_h; (void)color;
	return -1;
}

int rga_copy(void *src_vaddr, int src_w, int src_h,
	     void *dst_vaddr, int dst_w, int dst_h)
{
	(void)src_vaddr; (void)src_w; (void)src_h;
	(void)dst_vaddr; (void)dst_w; (void)dst_h;
	return -1;
}

int rga_blend(void *fg_vaddr, int fg_w, int fg_h,
	      void *bg_vaddr, int bg_w, int bg_h)
{
	(void)fg_vaddr; (void)fg_w; (void)fg_h;
	(void)bg_vaddr; (void)bg_w; (void)bg_h;
	return -1;
}
