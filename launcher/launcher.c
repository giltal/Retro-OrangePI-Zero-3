/*
 * retroopi_launcher - Custom retro gaming launcher
 *
 * Ported from RetroBPI_M2M, which ported it from the LyraZeroW SuperRetroPack
 * launcher. Differences along the way:
 *   - RGA2 2D acceleration is gone (Rockchip-only); see gfx_accel.c
 *   - the KMS device is discovered, not hardcoded to card0, because the GPU
 *     (lima there, panfrost here) also registers a DRM node
 *   - output geometry is a build-time define, and the HDMI mode is chosen to
 *     match it rather than taken from the display's preference
 *   - layout scales with SCREEN_HEIGHT (it was tuned at 480 lines)
 *
 * Platform: Orange Pi Zero3 (Allwinner H618, 4x Cortex-A53, Mali-G31 MP2)
 * Display:  HDMI via DRM (XRGB8888, single primary plane)
 * Input:    PS3 controller via evdev (generic HID)
 *
 * Inspired by MinUI (shauninman) and OnionUI for Miyoo Mini.
 * Same approach: software-rendered 2D UI on a GPU-less SoC with
 * hardware 2D acceleration available for blitting/scaling.
 *
 * Architecture:
 *   DRM dumb buffers -> mmap -> CPU/RGA2 render -> drmModeSetCrtc
 *   SDL2_ttf  for text rendering (software surfaces)
 *   SDL2_image for PNG thumbnails (software surfaces)
 *   evdev for gamepad input
 *
 * Copyright (c) 2026. Licensed under MIT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <linux/input.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>

#include "gfx_accel.h"

/* =========================================================================
 * Configuration
 * ========================================================================= */

/*
 * Output geometry. Set from the Makefile (SCREEN_W / SCREEN_H, which Buildroot
 * takes from BR2_PACKAGE_RETROOPI_LAUNCHER_SCREEN_W/H):
 *   1280x720  -> HDMI 720p, the current target
 *   1920x1080 -> HDMI 1080p, later
 *
 * TODO: query the mode from DRM at runtime instead. ~50 layout constants
 * derive from these, so that is a real refactor, not a rename.
 */
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH    1280
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT   720
#endif
#define SCREEN_BPP      32
#define SCREEN_STRIDE    (SCREEN_WIDTH * 4)

/*
 * Every size below was tuned on an 800x480 panel. UI_SCALE keeps the same
 * proportions at other heights: x1.5 at 720p, x2.25 at 1080p. It scales by
 * height only, because both HDMI targets are 16:9 and the extra width over
 * 5:3 simply gives the ROM names more room.
 */
#define UI_SCALE(px)    (((px) * SCREEN_HEIGHT + 240) / 480)

#define ROMS_PATH       "/opt/roms"
#define CORES_PATH      "/usr/lib/libretro"
#define RETROARCH_BIN   "/usr/bin/retroarch"
#define STATES_DIR      "/opt/roms/_system/states"
#define MAX_SLOTS       10  /* Save state slots 0-9 */
#define FONT_PATH       "/usr/share/fonts/launcher.ttf"
#define FONT_SIZE_LIST  UI_SCALE(22)
#define FONT_SIZE_HEADER UI_SCALE(26)
#define FONT_SIZE_SMALL UI_SCALE(16)
#define FONT_SIZE_JUMP  UI_SCALE(96)   /* letter-jump overlay */

/* UI Layout */
#define HEADER_HEIGHT   UI_SCALE(44)
#define FOOTER_HEIGHT   UI_SCALE(36)
#define ITEM_HEIGHT     UI_SCALE(38)
#define PADDING_X       UI_SCALE(20)
#define PADDING_Y       UI_SCALE(6)
#define LIST_TOP        (HEADER_HEIGHT + 4)
#define LIST_BOTTOM     (SCREEN_HEIGHT - FOOTER_HEIGHT)
#define MAX_VISIBLE     ((LIST_BOTTOM - LIST_TOP) / ITEM_HEIGHT)

/* Persistent data paths */
#define DATA_DIR        "/opt/roms/_system"
#define FAVORITES_FILE  DATA_DIR "/favorites.txt"
#define RECENTS_FILE    DATA_DIR "/recents.txt"
#define STATE_FILE      DATA_DIR "/state.txt"
#define THEME_FILE      DATA_DIR "/theme.txt"
#define MAX_RECENTS     50
#define MAX_FAVORITES   200

/* Additional button masks */
#define BTN_X_MASK       (1 << 10)  /* Toggle favorite (Triangle on PS3) */
#define BTN_Y_MASK       (1 << 11)  /* Unused (Square on PS3) */

typedef uint32_t pixel_t;

/*
 * PANEL_ROTATION: degrees the OUTPUT must be rotated to appear upright,
 * because of how the panel is physically mounted. Set at build time from
 * BR2_PACKAGE_RETROBPI_LAUNCHER_PANEL_ROTATION; the hardware fact itself is
 * recorded as `rotation = <180>` in the panel's device tree node.
 *
 * Deliberately NOT a Settings item: the panel's mounting is not a user
 * preference, and a wrong value inverts the very menu you would need to
 * navigate to undo it.
 *
 * Only 0 and 180 are implemented. 180 is the cheap case -- it is a reversal
 * of pixel order with no resampling and no change of stride, so it costs one
 * linear copy. 90/270 would swap width and height and need a real transpose.
 */
#ifndef PANEL_ROTATION
#define PANEL_ROTATION 0
#endif
#if PANEL_ROTATION != 0 && PANEL_ROTATION != 180
#error "PANEL_ROTATION: only 0 and 180 are implemented"
#endif

/* =========================================================================
 * Color Themes (XRGB8888)
 * ========================================================================= */

typedef struct {
    const char *name;
    pixel_t bg;
    pixel_t header_bg;
    pixel_t footer_bg;
    pixel_t text;
    pixel_t text_dim;
    pixel_t highlight;
    pixel_t highlight2;
    pixel_t divider;
    pixel_t accent;
    pixel_t star;       /* Favorite star color */
} Theme;

static const Theme g_themes[] = {
    { "Midnight",
      0x00101828, 0x001e293b, 0x000f172a, 0x00e2e8f0,
      0x0064748b, 0x003b82f6, 0x002563eb, 0x001e293b,
      0x0038bdf8, 0x00fbbf24 },
    { "Emerald",
      0x00022c22, 0x00064e3b, 0x00012a20, 0x00d1fae5,
      0x006ee7b7, 0x0010b981, 0x00059669, 0x00064e3b,
      0x0034d399, 0x00fbbf24 },
    { "Crimson",
      0x001c1017, 0x002d1a24, 0x00150c12, 0x00fce7f3,
      0x00f9a8d4, 0x00ec4899, 0x00db2777, 0x002d1a24,
      0x00f472b6, 0x00fbbf24 },
    { "Amber",
      0x001a1400, 0x002d2300, 0x00131000, 0x00fef3c7,
      0x00fcd34d, 0x00f59e0b, 0x00d97706, 0x002d2300,
      0x00fbbf24, 0x00f59e0b },
    { "Slate",
      0x000f172a, 0x001e293b, 0x00020617, 0x00f1f5f9,
      0x0094a3b8, 0x00475569, 0x00334155, 0x001e293b,
      0x00cbd5e1, 0x00fbbf24 },
    { "Ocean",
      0x000c1222, 0x001a2744, 0x00060c18, 0x00bae6fd,
      0x0038bdf8, 0x000ea5e9, 0x000284c7, 0x001a2744,
      0x007dd3fc, 0x00fbbf24 },
    { "Purple",
      0x001a0533, 0x002e1065, 0x00120226, 0x00f5d0fe,
      0x00d946ef, 0x00a855f7, 0x009333ea, 0x002e1065,
      0x00c084fc, 0x00fbbf24 },
    { "Retro Green",
      0x00001100, 0x00002200, 0x00000a00, 0x0033ff33,
      0x00228822, 0x00116611, 0x00005500, 0x00002200,
      0x0044ff44, 0x00ffff00 },
    { "OLED Black",
      0x00000000, 0x00101010, 0x00000000, 0x00e0e0e0,
      0x00606060, 0x00ffffff, 0x00d0d0d0, 0x00181818,
      0x00ffffff, 0x00fbbf24 },
};
#define NUM_THEMES (sizeof(g_themes) / sizeof(g_themes[0]))

static int g_current_theme = 0;
static int g_volume = 40;  /* 0-100%; see volume_apply() for the mapping.
                            * Was 80, which is uncomfortably loud on a fresh
                            * card: game audio is mastered near full scale,
                            * unlike the menu, and the speaker amp adds gain
                            * on top. Measured in practice, ~25-40% is a
                            * normal listening level. Only applies when there
                            * is no saved state.txt to override it. */

/* Active theme colors (updated when theme changes) */
static pixel_t COL_BG;
static pixel_t COL_HEADER_BG;
static pixel_t COL_FOOTER_BG;
static pixel_t COL_TEXT;
static pixel_t COL_TEXT_DIM;
static pixel_t COL_HIGHLIGHT;
static pixel_t COL_HIGHLIGHT2;
static pixel_t COL_DIVIDER;
static pixel_t COL_SYSTEM_ICON;
static pixel_t COL_STAR;

static void theme_apply(int idx)
{
    if (idx < 0 || idx >= (int)NUM_THEMES) idx = 0;
    g_current_theme = idx;
    const Theme *t = &g_themes[idx];
    COL_BG          = t->bg;
    COL_HEADER_BG   = t->header_bg;
    COL_FOOTER_BG   = t->footer_bg;
    COL_TEXT         = t->text;
    COL_TEXT_DIM     = t->text_dim;
    COL_HIGHLIGHT    = t->highlight;
    COL_HIGHLIGHT2   = t->highlight2;
    COL_DIVIDER      = t->divider;
    COL_SYSTEM_ICON  = t->accent;
    COL_STAR         = t->star;
}

/* =========================================================================
 * System -> Core mapping
 * ========================================================================= */

typedef struct {
    const char *dir_name;      /* ROM subdirectory name */
    const char *core_file;     /* libretro core filename */
    const char *display_name;  /* Human-readable name */
    const char *extensions;    /* Supported file extensions (comma-separated) */
} SystemDef;

static const SystemDef g_systems[] = {
    { "nes",          "fceumm_libretro.so",             "Nintendo",          "nes,unf,unif" },
    { "snes",         "snes9x2005_libretro.so",         "Super Nintendo",    "smc,sfc,fig,bs" },
    { "gb",           "gambatte_libretro.so",            "Game Boy",          "gb,gbc" },
    { "gbc",          "gambatte_libretro.so",            "Game Boy Color",    "gbc,gb" },
    /*
     * GBA uses gpSP, NOT mGBA. mGBA is a pure interpreter -- it decodes and
     * executes every ARM7TDMI instruction in software -- and does not hold
     * 60 fps on a Cortex-A7 at this clock. gpSP JIT-compiles ARM7 to native
     * ARMv7 through an mmap'd dynarec cache; the Lyra project measured 3-5x on
     * the same silicon and that is what took GBA to full speed there.
     *
     * mGBA is still installed and can be selected by hand in RetroArch if a
     * game needs its better accuracy and can afford the speed.
     */
    { "gba",          "gpsp_libretro.so",                "Game Boy Advance",  "gba" },
    { "genesis",      "genesisplusgx_libretro.so",       "Sega Genesis",      "md,bin,gen,smd" },
    { "mastersystem",  "genesisplusgx_libretro.so",       "Master System",     "sms" },
    { "gamegear",      "genesisplusgx_libretro.so",       "Game Gear",         "gg" },
    { "atari2600",     "stella2023_libretro.so",          "Atari 2600",        "a26,bin" },
    { "atari7800",     "prosystem_libretro.so",           "Atari 7800",        "a78,bin" },
    { "atari800",      "atari800_libretro.so",            "Atari 800",         "atr,bas,bin,cas,xex,xfd,dcm,com,rom,atx" },
    { "pce",           "beetlepcefast_libretro.so",       "PC Engine",         "pce,cue" },
    { "pcesupergrafx", "beetlesupergrafx_libretro.so",    "SuperGrafx",        "pce,sgx" },
    { "zxspectrum",    "fuse_libretro.so",                "ZX Spectrum",       "tzx,tap,z80,rzx" },
    { "psx",           "pcsx_rearmed_libretro.so",        "PlayStation",       "bin,cue,img,mdf,pbp,cbn,chd" },
    { "doom",          "prboom_libretro.so",              "Doom",              "wad" },
    { "neogeo",        "fbalpha2012_libretro.so",         "Neo Geo",           "zip" },
    { "cps1",          "fbalpha2012_libretro.so",         "CPS-1",             "zip" },
    { "cps2",          "fbalpha2012_libretro.so",         "CPS-2",             "zip" },
    { "cps3",          "fbalpha2012_libretro.so",         "CPS-3",             "zip" },
    { "arcade",        "fbalpha2012_libretro.so",         "Arcade (FBA)",      "zip" },
    { "mame",          "mame2003plus_libretro.so",        "Arcade (MAME)",     "zip" },
    /*
     * N64 via parallel-n64 rather than mupen64plus-next: the Mali-400 MP2 is
     * OpenGL ES 2.0 only, with no GLES3 and no desktop GL, and mupen64plus-next
     * requires GLES3/GL3. parallel-n64 still carries the older glide64 and rice
     * renderers, which are GLES2. Expect this to be marginal on a 1.2 GHz A7
     * either way -- it is here to be measured, not because it is known good.
     */
    { "n64",           "paralleln64_libretro.so",         "Nintendo 64",       "n64,v64,z64,ndd" },
    /*
     * New on the Zero3: both need GLES3, which the Mali-G31 has and the
     * reference's Mali-400 did not. PPSSPP's assets live in
     * _system/bios/PPSSPP (copied there by S46ppsspp). Dreamcast games are
     * .chd/.cdi files, or a GDI set in its own subfolder -- see
     * find_disc_in_dir().
     */
    { "psp",           "ppsspp_libretro.so",              "PlayStation Portable", "iso,cso,chd,pbp,elf" },
    { "dreamcast",     "flycast_libretro.so",             "Dreamcast",         "chd,cdi,gdi,cue,m3u" },
    { NULL, NULL, NULL, NULL }
};

/* =========================================================================
 * Game Name Lookup (gamenames.txt per system directory)
 * ========================================================================= */

#define MAX_GAMENAMES 512
#define MAX_NAME    256

typedef struct {
    char shortname[64];
    char fullname[MAX_NAME];
} GameName;

static GameName g_gamenames[MAX_GAMENAMES];
static int g_gamenames_count = 0;
static char g_gamenames_system[MAX_NAME]; /* Currently loaded system dir */

static void gamenames_load(const char *sys_dir_path)
{
    /* Don't reload if same directory */
    if (strcmp(g_gamenames_system, sys_dir_path) == 0)
        return;

    g_gamenames_count = 0;
    strncpy(g_gamenames_system, sys_dir_path, MAX_NAME - 1);
    g_gamenames_system[MAX_NAME - 1] = '\0';

    char path[MAX_NAME];
    snprintf(path, sizeof(path), "%s/gamenames.txt", sys_dir_path);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[MAX_NAME + 64];
    while (fgets(line, sizeof(line), f) && g_gamenames_count < MAX_GAMENAMES) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *cr = strchr(line, '\r');
        if (cr) *cr = '\0';
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = '\0';

        GameName *gn = &g_gamenames[g_gamenames_count];
        strncpy(gn->shortname, line, sizeof(gn->shortname) - 1);
        gn->shortname[sizeof(gn->shortname) - 1] = '\0';
        strncpy(gn->fullname, eq + 1, MAX_NAME - 1);
        gn->fullname[MAX_NAME - 1] = '\0';
        g_gamenames_count++;
    }
    fclose(f);
    printf("GAMENAMES: loaded %d names from %s\n", g_gamenames_count, path);
}

/* Lookup a display name by ROM filename or full path */
static const char *gamenames_lookup(const char *filename)
{
    char shortname[64];
    const char *slash = strrchr(filename, '/');
    const char *base = slash ? slash + 1 : filename;
    const char *dot = strrchr(base, '.');
    int len = dot ? (int)(dot - base) : (int)strlen(base);
    if (len >= (int)sizeof(shortname)) len = (int)sizeof(shortname) - 1;
    memcpy(shortname, base, len);
    shortname[len] = '\0';

    for (int i = 0; i < g_gamenames_count; i++) {
        if (strcasecmp(g_gamenames[i].shortname, shortname) == 0)
            return g_gamenames[i].fullname;
    }
    return NULL;
}

/* Lookup with auto-load from ROM's parent directory */
static const char *gamenames_lookup_path(const char *rom_full_path)
{
    char dir[MAX_NAME];
    strncpy(dir, rom_full_path, MAX_NAME - 1);
    dir[MAX_NAME - 1] = '\0';
    char *last_slash = strrchr(dir, '/');
    if (last_slash) *last_slash = '\0';
    else return NULL;

    gamenames_load(dir);
    return gamenames_lookup(rom_full_path);
}

/* =========================================================================
 * DRM Display
 * ========================================================================= */

typedef struct {
    int fd;
    uint32_t crtc_id;
    uint32_t connector_id;
    drmModeModeInfo mode;

    /* Double-buffered dumb framebuffers */
    struct {
        uint32_t handle;
        uint32_t fb_id;
        uint32_t size;
        pixel_t *pixels;
    } fb[2];
    int front;  /* Index of currently displayed buffer */
} DrmDisplay;

static DrmDisplay g_drm;

static int drm_create_dumb_fb(int idx)
{
    struct drm_mode_create_dumb create = {
        .width  = SCREEN_WIDTH,
        .height = SCREEN_HEIGHT,
        .bpp    = SCREEN_BPP,
    };
    if (drmIoctl(g_drm.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        fprintf(stderr, "DRM: create dumb buffer failed: %s\n", strerror(errno));
        return -1;
    }
    g_drm.fb[idx].handle = create.handle;
    g_drm.fb[idx].size   = create.size;

    if (drmModeAddFB(g_drm.fd, SCREEN_WIDTH, SCREEN_HEIGHT, 24, SCREEN_BPP,
                     create.pitch, create.handle, &g_drm.fb[idx].fb_id) < 0) {
        fprintf(stderr, "DRM: addFB failed: %s\n", strerror(errno));
        return -1;
    }

    struct drm_mode_map_dumb map = { .handle = create.handle };
    if (drmIoctl(g_drm.fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        fprintf(stderr, "DRM: map dumb failed: %s\n", strerror(errno));
        return -1;
    }
    g_drm.fb[idx].pixels = mmap(0, create.size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, g_drm.fd, map.offset);
    if (g_drm.fb[idx].pixels == MAP_FAILED) {
        fprintf(stderr, "DRM: mmap failed: %s\n", strerror(errno));
        return -1;
    }
    memset(g_drm.fb[idx].pixels, 0, create.size);
    return 0;
}

static void drm_destroy_dumb_fb(int idx)
{
    if (g_drm.fb[idx].pixels && g_drm.fb[idx].pixels != MAP_FAILED)
        munmap(g_drm.fb[idx].pixels, g_drm.fb[idx].size);
    if (g_drm.fb[idx].fb_id)
        drmModeRmFB(g_drm.fd, g_drm.fb[idx].fb_id);
    if (g_drm.fb[idx].handle) {
        struct drm_mode_destroy_dumb destroy = { .handle = g_drm.fb[idx].handle };
        drmIoctl(g_drm.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    memset(&g_drm.fb[idx], 0, sizeof(g_drm.fb[idx]));
}

/*
 * Find the KMS-capable DRM device.
 *
 * The Lyra could hardcode card0 because the RK3506B has no GPU, so the display
 * controller was the only DRM device. The A33 has a Mali-400 and mainline
 * enables lima, which registers its own DRM node with no connectors or CRTCs.
 * Probe order between sun4i-drm and lima is not guaranteed, so card0 may well
 * be the GPU. Pick the first node that actually has a connector instead.
 */
static int drm_open_kms(void)
{
    char path[32];

    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        drmModeRes *res = drmModeGetResources(fd);
        if (res && res->count_connectors > 0 && res->count_crtcs > 0) {
            drmModeFreeResources(res);
            fprintf(stderr, "DRM: using %s\n", path);
            return fd;
        }
        if (res)
            drmModeFreeResources(res);
        close(fd);
    }

    fprintf(stderr, "DRM: no KMS-capable device found in /dev/dri\n");
    return -1;
}

#if PANEL_ROTATION == 180
/*
 * Rotated output renders into a plain buffer first, then gets copied into the
 * dumb buffer reversed. Rotating the dumb buffer in place would be cheaper,
 * but only if every frame is drawn in full -- any partial redraw on top of an
 * already-rotated buffer would compound the rotation. A separate render target
 * makes that class of bug impossible for 1.5 MB.
 */
static pixel_t *g_render_buf = NULL;
#endif

/*
 * Pick the connector mode that matches our framebuffer.
 *
 * On the DSI panel modes[0] was the panel's only mode, so "use the preferred
 * mode" was the same thing as "use 800x480". On HDMI, modes[0] is whatever the
 * TV prefers -- typically 1080p or 4K -- while our dumb buffers are
 * SCREEN_WIDTH x SCREEN_HEIGHT. setCrtc with a buffer smaller than the mode
 * fails with ENOSPC, so the size has to match exactly.
 *
 * Preference: progressive 60 Hz, then any progressive refresh, then anything
 * of the right size. 1280x720@60 is mandatory for every HDMI sink (CEA-861),
 * so the first tier should always hit for the 720p build.
 */
static int drm_pick_mode(const drmModeConnector *conn, drmModeModeInfo *out)
{
    int best = -1, best_score = -1;

    for (int i = 0; i < conn->count_modes; i++) {
        const drmModeModeInfo *m = &conn->modes[i];
        if (m->hdisplay != SCREEN_WIDTH || m->vdisplay != SCREEN_HEIGHT)
            continue;
        int score = 1;
        if (!(m->flags & DRM_MODE_FLAG_INTERLACE)) score += 2;
        if (m->vrefresh == 60)                     score += 4;
        if (score > best_score) { best_score = score; best = i; }
    }

    if (best < 0) {
        fprintf(stderr, "DRM: display offers no %dx%d mode. It offers:\n",
                SCREEN_WIDTH, SCREEN_HEIGHT);
        for (int i = 0; i < conn->count_modes; i++)
            fprintf(stderr, "DRM:   %dx%d@%d%s\n", conn->modes[i].hdisplay,
                    conn->modes[i].vdisplay, conn->modes[i].vrefresh,
                    (conn->modes[i].flags & DRM_MODE_FLAG_INTERLACE) ? "i" : "");
        return -1;
    }
    *out = conn->modes[best];
    return 0;
}

static int drm_init(void)
{
    memset(&g_drm, 0, sizeof(g_drm));
    g_drm.fd = drm_open_kms();
    if (g_drm.fd < 0)
        return -1;

    drmModeRes *res = drmModeGetResources(g_drm.fd);
    if (!res) {
        fprintf(stderr, "DRM: cannot get resources\n");
        return -1;
    }

    /*
     * Find a connected connector. HDMI is hot-pluggable: a TV that is still
     * waking up raises HPD some seconds after the board has booted, and until
     * then the connector reads "disconnected" with no modes. Poll for a while
     * instead of failing at once. Each drmModeGetConnector() re-probes, which
     * also re-reads the EDID.
     */
    drmModeConnector *conn = NULL;
    for (int tries = 0; tries < 20 && !conn; tries++) {
        for (int i = 0; i < res->count_connectors; i++) {
            conn = drmModeGetConnector(g_drm.fd, res->connectors[i]);
            if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
                break;
            if (conn) drmModeFreeConnector(conn);
            conn = NULL;
        }
        if (!conn) {
            if (tries == 0)
                fprintf(stderr, "DRM: no display connected yet, waiting...\n");
            usleep(500 * 1000);
        }
    }
    if (!conn) {
        fprintf(stderr, "DRM: no connected connector\n");
        drmModeFreeResources(res);
        return -1;
    }
    g_drm.connector_id = conn->connector_id;
    if (drm_pick_mode(conn, &g_drm.mode) < 0) {
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return -1;
    }

    /* Find CRTC */
    drmModeEncoder *enc = drmModeGetEncoder(g_drm.fd, conn->encoder_id);
    if (enc) {
        g_drm.crtc_id = enc->crtc_id;
        drmModeFreeEncoder(enc);
    } else {
        g_drm.crtc_id = res->crtcs[0];
    }

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    printf("DRM: mode %dx%d@%d @ CRTC %u\n",
           g_drm.mode.hdisplay, g_drm.mode.vdisplay, g_drm.mode.vrefresh,
           g_drm.crtc_id);

    /* Create double-buffered framebuffers */
    if (drm_create_dumb_fb(0) < 0 || drm_create_dumb_fb(1) < 0)
        return -1;

#if PANEL_ROTATION == 180
    g_render_buf = calloc((size_t)SCREEN_WIDTH * SCREEN_HEIGHT, sizeof(pixel_t));
    if (!g_render_buf) {
        fprintf(stderr, "DRM: cannot allocate rotation buffer\n");
        return -1;
    }
#endif

    g_drm.front = 0;

    /* Set initial CRTC */
    if (drmModeSetCrtc(g_drm.fd, g_drm.crtc_id, g_drm.fb[0].fb_id, 0, 0,
                       &g_drm.connector_id, 1, &g_drm.mode) < 0) {
        fprintf(stderr, "DRM: setCrtc failed: %s\n", strerror(errno));
        return -1;
    }

    printf("DRM: initialized, double-buffered %dx%d XRGB8888, rotation %d\n",
           SCREEN_WIDTH, SCREEN_HEIGHT, PANEL_ROTATION);
    return 0;
}

static pixel_t *drm_backbuffer(void)
{
#if PANEL_ROTATION == 180
    return g_render_buf;
#else
    return g_drm.fb[g_drm.front ^ 1].pixels;
#endif
}

static void drm_flip(void)
{
    int back = g_drm.front ^ 1;
#if PANEL_ROTATION == 180
    /* 180 degrees is exactly a reversal of pixel order. */
    const pixel_t *src = g_render_buf;
    pixel_t       *dst = g_drm.fb[back].pixels;
    size_t         n   = (size_t)SCREEN_WIDTH * SCREEN_HEIGHT;
    for (size_t i = 0; i < n; i++)
        dst[i] = src[n - 1 - i];
#endif
    drmModeSetCrtc(g_drm.fd, g_drm.crtc_id, g_drm.fb[back].fb_id, 0, 0,
                   &g_drm.connector_id, 1, &g_drm.mode);
    g_drm.front = back;
}

static void drm_cleanup(void)
{
#if PANEL_ROTATION == 180
    free(g_render_buf);
    g_render_buf = NULL;
#endif
    drm_destroy_dumb_fb(0);
    drm_destroy_dumb_fb(1);
    if (g_drm.fd > 0) close(g_drm.fd);
    g_drm.fd = 0;
}

/* =========================================================================
 * Software Graphics (CPU rendering to DRM framebuffer)
 * ========================================================================= */

static TTF_Font *g_font_list   = NULL;
static TTF_Font *g_font_header = NULL;
static TTF_Font *g_font_small  = NULL;
static TTF_Font *g_font_jump   = NULL;  /* letter-jump overlay */

/*
 * Symbol fallback fonts, one per size above.
 *
 * launcher.ttf has no symbols at all -- none of the gear, star, clock,
 * arrow or button glyphs the UI uses -- so every one of them rendered as
 * the font's "missing glyph" box, a square with an X in it. DejaVu Sans
 * (Buildroot's dejavu package) covers all of them, so gfx_draw_text() renders
 * any codepoint the main font lacks from here instead. Optional: without
 * the file the launcher still runs, with the boxes back.
 */
#define SYMBOL_FONT_PATH "/usr/share/fonts/dejavu/DejaVuSans.ttf"
static TTF_Font *g_sym_list   = NULL;
static TTF_Font *g_sym_header = NULL;
static TTF_Font *g_sym_small  = NULL;
static TTF_Font *g_sym_jump   = NULL;

static TTF_Font *gfx_symbol_font_for(TTF_Font *font)
{
    if (font == g_font_list)   return g_sym_list;
    if (font == g_font_header) return g_sym_header;
    if (font == g_font_small)  return g_sym_small;
    if (font == g_font_jump)   return g_sym_jump;
    return NULL;
}

static int gfx_init(void)
{
    if (TTF_Init() < 0) {
        fprintf(stderr, "GFX: TTF_Init failed: %s\n", TTF_GetError());
        return -1;
    }
    g_font_list = TTF_OpenFont(FONT_PATH, FONT_SIZE_LIST);
    if (!g_font_list) {
        fprintf(stderr, "GFX: cannot open font %s: %s\n", FONT_PATH, TTF_GetError());
        return -1;
    }
    g_font_header = TTF_OpenFont(FONT_PATH, FONT_SIZE_HEADER);
    g_font_small  = TTF_OpenFont(FONT_PATH, FONT_SIZE_SMALL);
    g_font_jump   = TTF_OpenFont(FONT_PATH, FONT_SIZE_JUMP);
    if (!g_font_header || !g_font_small) {
        fprintf(stderr, "GFX: cannot open font sizes\n");
        return -1;
    }

    g_sym_list   = TTF_OpenFont(SYMBOL_FONT_PATH, FONT_SIZE_LIST);
    g_sym_header = TTF_OpenFont(SYMBOL_FONT_PATH, FONT_SIZE_HEADER);
    g_sym_small  = TTF_OpenFont(SYMBOL_FONT_PATH, FONT_SIZE_SMALL);
    g_sym_jump   = TTF_OpenFont(SYMBOL_FONT_PATH, FONT_SIZE_JUMP);
    if (!g_sym_list)
        fprintf(stderr, "GFX: no symbol font %s -- UI symbols will show as boxes\n",
                SYMBOL_FONT_PATH);

    int img_flags = IMG_INIT_PNG;
    if (!(IMG_Init(img_flags) & img_flags)) {
        fprintf(stderr, "GFX: IMG_Init failed: %s\n", IMG_GetError());
        /* Non-fatal: thumbnails won't work */
    }

    printf("GFX: fonts loaded, SDL2_image ready\n");
    return 0;
}

static void gfx_cleanup(void)
{
    if (g_sym_jump)    TTF_CloseFont(g_sym_jump);
    if (g_sym_small)   TTF_CloseFont(g_sym_small);
    if (g_sym_header)  TTF_CloseFont(g_sym_header);
    if (g_sym_list)    TTF_CloseFont(g_sym_list);
    g_sym_list = g_sym_header = g_sym_small = g_sym_jump = NULL;
    if (g_font_jump)   TTF_CloseFont(g_font_jump);
    if (g_font_small)  TTF_CloseFont(g_font_small);
    if (g_font_header) TTF_CloseFont(g_font_header);
    if (g_font_list)   TTF_CloseFont(g_font_list);
    IMG_Quit();
    TTF_Quit();
}

/* Fill a rectangle in the backbuffer */
static void gfx_fill_rect(int x, int y, int w, int h, pixel_t color)
{
    pixel_t *fb = drm_backbuffer();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_WIDTH)  w = SCREEN_WIDTH - x;
    if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    /* Try RGA2 hardware fill (min 2x2) */
    if (w >= 2 && h >= 2 &&
        rga_fill_rect(fb, SCREEN_WIDTH, SCREEN_HEIGHT, x, y, w, h, color) == 0)
        return;

    /* CPU fallback */
    for (int row = y; row < y + h; row++) {
        pixel_t *dst = fb + row * SCREEN_WIDTH + x;
        for (int col = 0; col < w; col++)
            dst[col] = color;
    }
}

/* Fill entire backbuffer with a color */
static void gfx_clear(pixel_t color)
{
    pixel_t *fb = drm_backbuffer();

    /* Try RGA2 hardware clear */
    if (rga_clear(fb, SCREEN_WIDTH, SCREEN_HEIGHT, color) == 0)
        return;

    /* CPU fallback */
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++)
        fb[i] = color;
}

/* Alpha-blend an SDL_Surface (ARGB8888) onto the framebuffer */
static void gfx_blit_surface(SDL_Surface *surf, int dst_x, int dst_y)
{
    if (!surf) return;
    pixel_t *fb = drm_backbuffer();

    /* Ensure ARGB8888 format */
    SDL_Surface *conv = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ARGB8888, 0);
    if (!conv) return;

    SDL_LockSurface(conv);
    uint32_t *src = (uint32_t *)conv->pixels;
    int src_pitch = conv->pitch / 4;

    for (int sy = 0; sy < conv->h; sy++) {
        int dy = dst_y + sy;
        if (dy < 0) continue;
        if (dy >= SCREEN_HEIGHT) break;

        for (int sx = 0; sx < conv->w; sx++) {
            int dx = dst_x + sx;
            if (dx < 0) continue;
            if (dx >= SCREEN_WIDTH) break;

            uint32_t spx = src[sy * src_pitch + sx];
            uint8_t a = (spx >> 24) & 0xFF;
            if (a == 0) continue;

            pixel_t *dpx = &fb[dy * SCREEN_WIDTH + dx];
            if (a == 255) {
                *dpx = spx & 0x00FFFFFF;
            } else {
                uint8_t inv = 255 - a;
                uint8_t r = (((spx >> 16) & 0xFF) * a + ((*dpx >> 16) & 0xFF) * inv) / 255;
                uint8_t g = (((spx >> 8)  & 0xFF) * a + ((*dpx >> 8)  & 0xFF) * inv) / 255;
                uint8_t b = (((spx)       & 0xFF) * a + ((*dpx)       & 0xFF) * inv) / 255;
                *dpx = (r << 16) | (g << 8) | b;
            }
        }
    }
    SDL_UnlockSurface(conv);
    SDL_FreeSurface(conv);
}

/* Render text and blit to framebuffer. Returns rendered width. */
/* Render one run of text with one font, clipped to max_width (0 = no limit). */
static int gfx_draw_text_run(TTF_Font *font, const char *text, int x, int y,
                             SDL_Color sdl_color, int max_width)
{
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, sdl_color);
    if (!surf) return 0;

    /* Clip if wider than max_width */
    if (max_width > 0 && surf->w > max_width) {
        SDL_Rect clip = { 0, 0, max_width, surf->h };
        SDL_Surface *clipped = SDL_CreateRGBSurface(0, max_width, surf->h, 32,
            0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        if (clipped) {
            SDL_BlitSurface(surf, &clip, clipped, NULL);
            SDL_FreeSurface(surf);
            surf = clipped;
        }
    }

    int w = surf->w;
    gfx_blit_surface(surf, x, y);
    SDL_FreeSurface(surf);
    return w;
}

/* Decode one UTF-8 codepoint at s; returns its length in bytes (>= 1). */
static int utf8_next(const char *s, Uint32 *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && u[1]) {
        *cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); return 2;
    }
    if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) {
        *cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
        *cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) |
              ((u[2] & 0x3F) << 6) | (u[3] & 0x3F); return 4;
    }
    *cp = 0xFFFD; return 1;     /* malformed: consume one byte */
}

/*
 * Draw text, taking any glyph the main font lacks from the symbol font.
 *
 * The string is split into runs of "main font has it" and "it does not", and
 * each run is rendered with its own font. Runs are aligned on a common
 * baseline: the fonts have different ascents, so a plain same-y blit would
 * put the symbols visibly high or low.
 */
static int gfx_draw_text(TTF_Font *font, const char *text, int x, int y,
                          pixel_t color, int max_width)
{
    if (!text || !text[0]) return 0;
    SDL_Color sdl_color = {
        (color >> 16) & 0xFF,
        (color >> 8)  & 0xFF,
        (color)       & 0xFF,
        255
    };

    /* Fast path: pure ASCII, or no symbol font -- one run, as before. */
    TTF_Font *sym = gfx_symbol_font_for(font);
    const char *p = text;
    while (*p && !(*p & 0x80)) p++;
    if (!*p || !sym)
        return gfx_draw_text_run(font, text, x, y, sdl_color, max_width);

    char run[MAX_NAME];
    int drawn = 0;
    const int asc = TTF_FontAscent(font);

    p = text;
    while (*p) {
        /* Collect a run of codepoints that all want the same font. */
        Uint32 cp;
        int n = utf8_next(p, &cp);
        TTF_Font *rf = (cp < 0x80 || TTF_GlyphIsProvided32(font, cp)) ? font : sym;
        size_t len = 0;
        while (*p) {
            n = utf8_next(p, &cp);
            TTF_Font *f = (cp < 0x80 || TTF_GlyphIsProvided32(font, cp)) ? font : sym;
            if (f != rf || len + n >= sizeof(run)) break;
            memcpy(run + len, p, n);
            len += n;
            p += n;
        }
        run[len] = 0;

        int room = max_width > 0 ? max_width - drawn : 0;
        if (max_width > 0 && room <= 0) break;
        int dy = asc - TTF_FontAscent(rf);
        drawn += gfx_draw_text_run(rf, run, x + drawn, y + dy, sdl_color, room);
    }
    return drawn;
}

/* Draw text centered horizontally */
static void gfx_draw_text_centered(TTF_Font *font, const char *text, int y,
                                    pixel_t color)
{
    if (!text || !text[0]) return;
    int tw, th;
    TTF_SizeUTF8(font, text, &tw, &th);
    gfx_draw_text(font, text, (SCREEN_WIDTH - tw) / 2, y, color, 0);
}

/* Draw a rounded-ish pill highlight (simple: just a filled rect with 1px inset) */
static void gfx_draw_pill(int x, int y, int w, int h, pixel_t color)
{
    /* Main body */
    gfx_fill_rect(x + 2, y, w - 4, h, color);
    /* Left/right rounded edges (just 2px narrower top/bottom) */
    gfx_fill_rect(x, y + 2, 2, h - 4, color);
    gfx_fill_rect(x + w - 2, y + 2, 2, h - 4, color);
    /* Corner pixels */
    gfx_fill_rect(x + 1, y + 1, 1, 1, color);
    gfx_fill_rect(x + w - 2, y + 1, 1, 1, color);
    gfx_fill_rect(x + 1, y + h - 2, 1, 1, color);
    gfx_fill_rect(x + w - 2, y + h - 2, 1, 1, color);
}

/* Draw a filled 5-pointed star */
static void gfx_draw_star(int cx, int cy, int outer_r, int inner_r, pixel_t color)
{
    float vx[10], vy[10];
    for (int i = 0; i < 5; i++) {
        float a_out = (float)(-M_PI / 2.0 + i * 2.0 * M_PI / 5.0);
        float a_in  = (float)(-M_PI / 2.0 + (i + 0.5) * 2.0 * M_PI / 5.0);
        vx[i * 2]     = cx + outer_r * cosf(a_out);
        vy[i * 2]     = cy + outer_r * sinf(a_out);
        vx[i * 2 + 1] = cx + inner_r * cosf(a_in);
        vy[i * 2 + 1] = cy + inner_r * sinf(a_in);
    }

    int ymin = cy - outer_r;
    int ymax = cy + outer_r;
    if (ymin < 0) ymin = 0;
    if (ymax >= SCREEN_HEIGHT) ymax = SCREEN_HEIGHT - 1;

    pixel_t *fb = drm_backbuffer();

    for (int y = ymin; y <= ymax; y++) {
        float xints[20];
        int nints = 0;
        float fy = y + 0.5f;
        for (int i = 0; i < 10; i++) {
            int j = (i + 1) % 10;
            if ((vy[i] <= fy && vy[j] > fy) || (vy[j] <= fy && vy[i] > fy)) {
                float t = (fy - vy[i]) / (vy[j] - vy[i]);
                xints[nints++] = vx[i] + t * (vx[j] - vx[i]);
            }
        }
        /* Sort intersections */
        for (int a = 0; a < nints - 1; a++)
            for (int b = a + 1; b < nints; b++)
                if (xints[b] < xints[a]) {
                    float tmp = xints[a]; xints[a] = xints[b]; xints[b] = tmp;
                }
        /* Fill between pairs */
        for (int i = 0; i + 1 < nints; i += 2) {
            int x0 = (int)(xints[i] + 0.5f);
            int x1 = (int)(xints[i + 1]);
            if (x0 < 0) x0 = 0;
            if (x1 >= SCREEN_WIDTH) x1 = SCREEN_WIDTH - 1;
            pixel_t *row = fb + y * SCREEN_WIDTH;
            for (int x = x0; x <= x1; x++)
                row[x] = color;
        }
    }
}

/* =========================================================================
 * Input (evdev)
 * ========================================================================= */

/* Button bitmask */
#define BTN_UP_MASK      (1 << 0)
#define BTN_DOWN_MASK    (1 << 1)
#define BTN_LEFT_MASK    (1 << 2)
#define BTN_RIGHT_MASK   (1 << 3)
#define BTN_A_MASK       (1 << 4)  /* Confirm (Cross on PS3) */
#define BTN_B_MASK       (1 << 5)  /* Back (Circle on PS3) */
#define BTN_START_MASK   (1 << 6)
#define BTN_SELECT_MASK  (1 << 7)
#define BTN_L1_MASK      (1 << 8)
#define BTN_R1_MASK      (1 << 9)

typedef struct {
    int fd;
    uint32_t held;      /* Currently held buttons */
    uint32_t pressed;   /* Just pressed this frame */
    uint32_t released;  /* Just released this frame */
} InputState;

static InputState g_input;

/*
 * PS3 controller generic HID button mapping:
 * evdev button index → button code mapping:
 *   BTN_GAMEPAD(0x130)+0  = Select
 *   BTN_GAMEPAD+3  = Start
 *   BTN_GAMEPAD+4  = D-Up
 *   BTN_GAMEPAD+5  = D-Right
 *   BTN_GAMEPAD+6  = D-Down
 *   BTN_GAMEPAD+7  = D-Left
 *   BTN_GAMEPAD+10 = L1
 *   BTN_GAMEPAD+11 = R1
 *   BTN_GAMEPAD+12 = Triangle
 *   BTN_GAMEPAD+13 = Circle
 *   BTN_GAMEPAD+14 = Cross
 *   BTN_GAMEPAD+15 = Square
 */

static uint32_t evdev_to_mask(uint16_t code)
{
    /* Generic HID joystick buttons (BTN_TRIGGER = 0x120)
     * PS3 without hid-sony: 0=Select 1=L3 2=R3 3=Start
     * 4=Up 5=Right 6=Down 7=Left 8=L2 9=R2 10=L1 11=R1
     * 12=Triangle 13=Circle 14=Cross 15=Square */
    switch (code) {
    case 0x120 + 4:  return BTN_UP_MASK;     /* D-Up */
    case 0x120 + 6:  return BTN_DOWN_MASK;   /* D-Down */
    case 0x120 + 7:  return BTN_LEFT_MASK;   /* D-Left */
    case 0x120 + 5:  return BTN_RIGHT_MASK;  /* D-Right */
    case 0x120 + 14: return BTN_A_MASK;      /* Cross = confirm */
    case 0x120 + 13: return BTN_B_MASK;      /* Circle = back */
    case 0x120 + 3:  return BTN_START_MASK;  /* Start */
    case 0x120 + 0:  return BTN_SELECT_MASK; /* Select */
    case 0x120 + 10: return BTN_L1_MASK;     /* L1 */
    case 0x120 + 11: return BTN_R1_MASK;     /* R1 */
    case 0x120 + 12: return BTN_X_MASK;      /* Triangle = toggle fav */
    case 0x120 + 15: return BTN_Y_MASK;      /* Square */
    }

    /* Standard gamepad buttons (in case hid-sony is loaded) */
    switch (code) {
    case BTN_DPAD_UP:    return BTN_UP_MASK;
    case BTN_DPAD_DOWN:  return BTN_DOWN_MASK;
    case BTN_DPAD_LEFT:  return BTN_LEFT_MASK;
    case BTN_DPAD_RIGHT: return BTN_RIGHT_MASK;
    case BTN_SOUTH:      return BTN_A_MASK;
    case BTN_EAST:       return BTN_B_MASK;
    case BTN_START:      return BTN_START_MASK;
    case BTN_SELECT:     return BTN_SELECT_MASK;
    case BTN_TL:         return BTN_L1_MASK;
    case BTN_TR:         return BTN_R1_MASK;
    case BTN_NORTH:      return BTN_X_MASK;
    case BTN_WEST:       return BTN_Y_MASK;
    }
    return 0;
}

static int input_find_gamepad(void)
{
    char path[64];
    int fallback_fd = -1;

    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        char name[256] = "";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);

        /* Check key capabilities */
        unsigned long keybits[16] = {0};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);

        /* BTN_GAMEPAD (0x130) — definitive gamepad identification */
        int btn_gamepad_word = 0x130 / (sizeof(long) * 8);
        int btn_gamepad_bit  = 0x130 % (sizeof(long) * 8);
        if (keybits[btn_gamepad_word] & (1UL << btn_gamepad_bit)) {
            printf("INPUT: found gamepad '%s' at %s\n", name, path);
            if (fallback_fd >= 0) close(fallback_fd);
            return fd;
        }

        /* BTN_SOUTH (0x130 already checked above), BTN_A (0x130),
         * also check BTN_JOYSTICK (0x120) for generic HID gamepads */
        int btn_joy_word = 0x120 / (sizeof(long) * 8);
        int btn_joy_bit  = 0x120 % (sizeof(long) * 8);
        if (keybits[btn_joy_word] & (1UL << btn_joy_bit)) {
            printf("INPUT: found joystick '%s' at %s\n", name, path);
            if (fallback_fd >= 0) close(fallback_fd);
            return fd;
        }

        /* Has EV_KEY and EV_ABS but not touchscreen — keep as fallback */
        unsigned long evbits[2] = {0};
        ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
        if ((evbits[0] & (1 << EV_KEY)) && (evbits[0] & (1 << EV_ABS))) {
            /* Skip touchscreens: check for BTN_TOUCH (0x14a) without BTN_GAMEPAD */
            int btn_touch_word = BTN_TOUCH / (sizeof(long) * 8);
            int btn_touch_bit  = BTN_TOUCH % (sizeof(long) * 8);
            if (keybits[btn_touch_word] & (1UL << btn_touch_bit)) {
                /* Has BTN_TOUCH — this is a touchscreen, skip */
                close(fd);
                continue;
            }
            /* Keep as fallback */
            if (fallback_fd >= 0) close(fallback_fd);
            fallback_fd = fd;
            printf("INPUT: fallback candidate '%s' at %s\n", name, path);
            continue;
        }

        close(fd);
    }

    if (fallback_fd >= 0)
        return fallback_fd;

    return -1;
}

static int input_init(void)
{
    memset(&g_input, 0, sizeof(g_input));
    g_input.fd = input_find_gamepad();
    if (g_input.fd < 0) {
        fprintf(stderr, "INPUT: no gamepad found\n");
        return -1;
    }
    return 0;
}

static void input_poll(void)
{
    uint32_t prev = g_input.held;
    struct input_event ev;
    ssize_t n;

    if (g_input.fd < 0) {
        /* Try to find a gamepad again */
        g_input.fd = input_find_gamepad();
        if (g_input.fd < 0)
            goto out;
    }

    while ((n = read(g_input.fd, &ev, sizeof(ev))) == sizeof(ev)) {
        if (ev.type == EV_KEY) {
            uint32_t mask = evdev_to_mask(ev.code);
            if (mask) {
                if (ev.value) /* pressed or repeat */
                    g_input.held |= mask;
                else
                    g_input.held &= ~mask;
            }
        }
        /* Handle d-pad as axis (ABS_HAT0X/Y or ABS_X/Y analog stick) */
        else if (ev.type == EV_ABS) {
            if (ev.code == ABS_HAT0X) {
                g_input.held &= ~(BTN_LEFT_MASK | BTN_RIGHT_MASK);
                if (ev.value < 0)      g_input.held |= BTN_LEFT_MASK;
                else if (ev.value > 0) g_input.held |= BTN_RIGHT_MASK;
            }
            else if (ev.code == ABS_HAT0Y) {
                g_input.held &= ~(BTN_UP_MASK | BTN_DOWN_MASK);
                if (ev.value < 0)      g_input.held |= BTN_UP_MASK;
                else if (ev.value > 0) g_input.held |= BTN_DOWN_MASK;
            }
            /* Left analog stick as d-pad (0-255, center 128) */
            else if (ev.code == ABS_X) {
                g_input.held &= ~(BTN_LEFT_MASK | BTN_RIGHT_MASK);
                if (ev.value < 64)       g_input.held |= BTN_LEFT_MASK;
                else if (ev.value > 192) g_input.held |= BTN_RIGHT_MASK;
            }
            else if (ev.code == ABS_Y) {
                g_input.held &= ~(BTN_UP_MASK | BTN_DOWN_MASK);
                if (ev.value < 64)       g_input.held |= BTN_UP_MASK;
                else if (ev.value > 192) g_input.held |= BTN_DOWN_MASK;
            }
        }
    }

    /* Detect device disconnection (stale fd) */
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        fprintf(stderr, "INPUT: device lost (errno=%d), will re-scan\n", errno);
        close(g_input.fd);
        g_input.fd = -1;
        g_input.held = 0;
    }

out:
    g_input.pressed  = g_input.held & ~prev;
    g_input.released = prev & ~g_input.held;
}

static void input_cleanup(void)
{
    if (g_input.fd >= 0) close(g_input.fd);
    g_input.fd = -1;
}

/* =========================================================================
 * Touch Input
 * ========================================================================= */

typedef struct {
    int fd;
    int x, y;
    bool touching;
    bool just_down;
    bool just_up;
} TouchState;

static TouchState g_touch;

static int touch_init(void)
{
    memset(&g_touch, 0, sizeof(g_touch));
    g_touch.fd = -1;

    char path[64];
    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        unsigned long keybits[16] = {0};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);

        int word = BTN_TOUCH / (sizeof(long) * 8);
        int bit  = BTN_TOUCH % (sizeof(long) * 8);

        if (keybits[word] & (1UL << bit)) {
            char name[256] = "";
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            printf("TOUCH: found '%s' at %s\n", name, path);
            g_touch.fd = fd;
            return 0;
        }
        close(fd);
    }
    printf("TOUCH: no touchscreen found (search uses gamepad only)\n");
    return 0;
}

static void touch_poll(void)
{
    g_touch.just_down = false;
    g_touch.just_up = false;
    if (g_touch.fd < 0) return;

    struct input_event ev;
    while (read(g_touch.fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS) {
            /*
             * Touch is NOT rotated, even though the display is.
             *
             * This is measured, not assumed. With the display rotated 180 and
             * the launcher logging what it received, tapping the corners gave:
             *
             *     viewer top-left      -> raw (8, 3)
             *     viewer bottom-right  -> raw (787, 452)
             *
             * So the digitizer's origin is already at the VIEWER's top-left --
             * it agrees with the rotated display and needs no transform. The
             * touch layer is evidently bonded in a different orientation from
             * the LCD, which is why "the panel is upside down" does not imply
             * "touch is upside down".
             *
             * An earlier version rotated these to match the framebuffer, on the
             * reasoning that the display rotation must apply to input too. That
             * was wrong and produced exactly inverted coordinates -- top-left
             * read as (791, 476). If the panel is ever remounted, re-measure
             * with the corner tap rather than deriving it: this axis does not
             * follow PANEL_ROTATION.
             */
            if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X)
                g_touch.x = ev.value;
            else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y)
                g_touch.y = ev.value;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value && !g_touch.touching) {
                g_touch.touching = true;
                g_touch.just_down = true;
            } else if (!ev.value && g_touch.touching) {
                g_touch.touching = false;
                g_touch.just_up = true;
            }
        }
    }
}

static void touch_cleanup(void)
{
    if (g_touch.fd >= 0) close(g_touch.fd);
    g_touch.fd = -1;
}

/* =========================================================================
 * Menu Data Model
 * ========================================================================= */

/*
 * Menu capacity. Was 256, which silently truncated: a folder with 663 Atari
 * 2600 ROMs listed only the first 256, while the system row still showed the
 * true count -- so 61% of the collection was unreachable with no indication
 * why. MenuEntry is ~524 bytes and g_menu is static (BSS, not stack), so
 * 4096 entries costs about 2.1 MB of a 512 MB machine.
 */
#define MAX_ENTRIES 4096

typedef enum {
    MENU_SYSTEMS,   /* Browsing system list */
    MENU_ROMS,      /* Browsing ROMs in a system */
    MENU_FAVORITES, /* Browsing favorites */
    MENU_RECENTS,   /* Browsing recently played */
    MENU_SETTINGS,  /* Settings screen */
} MenuMode;

typedef struct {
    char name[MAX_NAME];     /* Display name */
    char path[MAX_NAME];     /* Full path */
    int  system_idx;         /* Index into g_systems (for MENU_SYSTEMS) */
    int  rom_count;          /* ROM count (for system entries) */
    bool is_favorite;        /* Is this ROM a favorite? */
} MenuEntry;

typedef struct {
    MenuMode mode;
    MenuEntry entries[MAX_ENTRIES];
    int count;
    int selected;
    int scroll_top;

    /* Current system (when in MENU_ROMS mode) */
    int current_system;
    char system_display_name[MAX_NAME];
} Menu;

static Menu g_menu;

/* Favorites and recents data (used by menu_scan_systems) */
static char g_favorites[MAX_FAVORITES][MAX_NAME];
static int  g_favorites_count = 0;
static char g_recents[MAX_RECENTS][MAX_NAME];
static int  g_recents_count = 0;

/* Forward declarations */
static bool favorites_contains(const char *path);
static int  count_roms_in_dir(const char *dir_path);
static bool input_just_pressed_or_repeat(uint32_t mask);

/* Strip common ROM filename cruft for display */
static void clean_display_name(const char *filename, char *out, int out_size)
{
    /* Copy without extension */
    const char *dot = strrchr(filename, '.');
    int len = dot ? (int)(dot - filename) : (int)strlen(filename);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, filename, len);
    out[len] = '\0';

    /* Remove trailing region tags like (U), [!], etc. - keep them for now
     * Users can see the full name; stripping is optional */
}

static int entry_compare(const void *a, const void *b)
{
    return strcasecmp(((MenuEntry *)a)->name, ((MenuEntry *)b)->name);
}

/* Scan /roms/ for system directories that have ROMs and known cores */
/*
 * Which row of the systems list we descended from, so backing out returns to it.
 *
 * Keyed on system_idx, not the row number: the systems list is rebuilt from disk
 * every time menu_scan_systems() runs, and a system appears or disappears as its
 * folder gains or loses ROMs. A stored row index would quietly point at the
 * wrong system the moment that happened; system_idx is the index into the static
 * g_systems[] table, plus the sentinels -100/-101/-102 for Settings, Favorites
 * and Recents, and is stable across a rescan.
 */
#define SYSTEMS_RETURN_NONE  (-32768)
static int g_systems_return_id = SYSTEMS_RETURN_NONE;

static void menu_restore_systems_selection(void)
{
    if (g_systems_return_id == SYSTEMS_RETURN_NONE)
        return;

    for (int i = 0; i < g_menu.count; i++) {
        if (g_menu.entries[i].system_idx == g_systems_return_id) {
            g_menu.selected = i;
            break;
        }
    }
    /* Not found means that system lost its last ROM while we were away; leaving
     * the selection at the top is the honest outcome rather than guessing. */
    g_systems_return_id = SYSTEMS_RETURN_NONE;
    /* scroll_top is clamped by ui_draw(), so nothing to do here. */
}

static void menu_scan_systems(void)
{
    g_menu.mode = MENU_SYSTEMS;
    g_menu.count = 0;
    g_menu.selected = 0;
    g_menu.scroll_top = 0;

    /* Special entries at top: Settings, Favorites, Recently Played */
    {
        MenuEntry *e = &g_menu.entries[g_menu.count++];
        strncpy(e->name, "\xe2\x9a\x99  Settings", MAX_NAME - 1);
        e->path[0] = '\0';
        e->system_idx = -100; /* Sentinel: settings */
        e->rom_count = 0;
        e->is_favorite = false;
    }
    if (g_favorites_count > 0) {
        MenuEntry *e = &g_menu.entries[g_menu.count++];
        snprintf(e->name, MAX_NAME, "\xe2\x98\x85  Favorites (%d)", g_favorites_count);
        e->path[0] = '\0';
        e->system_idx = -101; /* Sentinel: favorites */
        e->rom_count = g_favorites_count;
        e->is_favorite = false;
    }
    if (g_recents_count > 0) {
        MenuEntry *e = &g_menu.entries[g_menu.count++];
        /* U+25F7 (clock face), not U+23F1 (stopwatch): DejaVu Sans, the
         * symbol fallback, has no stopwatch, so that one stayed a box. */
        snprintf(e->name, MAX_NAME, "\xe2\x97\xb7  Recently Played (%d)", g_recents_count);
        e->path[0] = '\0';
        e->system_idx = -102; /* Sentinel: recents */
        e->rom_count = g_recents_count;
        e->is_favorite = false;
    }

    DIR *d = opendir(ROMS_PATH);
    if (!d) {
        fprintf(stderr, "MENU: cannot open %s\n", ROMS_PATH);
        return;
    }

    /* Temporary array for systems to sort separately from special entries */
    int special_count = g_menu.count;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_menu.count < MAX_ENTRIES) {
        if (de->d_name[0] == '.') continue;

        /* Check if this directory matches a known system */
        int sys_idx = -1;
        for (int i = 0; g_systems[i].dir_name; i++) {
            if (strcasecmp(de->d_name, g_systems[i].dir_name) == 0) {
                sys_idx = i;
                break;
            }
        }
        if (sys_idx < 0) continue;

        /* Hide systems with no games. Uses count_roms_in_dir(), the same
         * definition of "a game" the ROM list uses, so a game FOLDER (a
         * Dreamcast GDI set) counts too. The check used to be its own loop
         * over plain files only, which hid a Dreamcast folder whose only
         * game was in a subfolder. */
        char syspath[MAX_NAME];
        snprintf(syspath, sizeof(syspath), "%s/%s", ROMS_PATH, de->d_name);
        if (count_roms_in_dir(syspath) == 0) continue;

        /* Check that the core exists */
        char corepath[MAX_NAME];
        snprintf(corepath, sizeof(corepath), "%s/%s",
                 CORES_PATH, g_systems[sys_idx].core_file);
        if (access(corepath, F_OK) != 0) continue;

        MenuEntry *e = &g_menu.entries[g_menu.count++];
        e->system_idx = sys_idx;
        e->is_favorite = false;
        strncpy(e->path, syspath, MAX_NAME - 1);

        /* Count ROMs and include in display name */
        int rc = count_roms_in_dir(syspath);
        e->rom_count = rc;
        snprintf(e->name, MAX_NAME, "%s (%d)", g_systems[sys_idx].display_name, rc);
    }
    closedir(d);

    /* Sort only the system entries (after special items) */
    qsort(&g_menu.entries[special_count], g_menu.count - special_count,
          sizeof(MenuEntry), entry_compare);
    printf("MENU: found %d systems\n", g_menu.count - special_count);
}

/* Scan ROMs in a system directory */
/*
 * Remember which ROM you were on, per system.
 *
 * menu_scan_roms() rebuilds the list from disk each time a system is opened, so
 * without this every visit starts at the top -- which with 663 Atari 2600 ROMs
 * means re-navigating from "1942" on every trip back.
 *
 * Keyed on the ROM path rather than its row index, for the same reason the
 * systems list is keyed on system_idx: the list is rebuilt from a directory that
 * can change between visits, and .bin entries are dropped when a matching .cue
 * exists, so row numbers are not stable. A path that has since vanished simply
 * is not found, and the list opens at the top.
 *
 * Session-only, deliberately: persisting it would mean writing a file per system
 * on every selection change, to a FAT partition, to save re-navigating after a
 * reboot -- which is not the annoyance being fixed here.
 */
#define MAX_TRACKED_SYSTEMS 64
static char g_last_rom[MAX_TRACKED_SYSTEMS][MAX_NAME];

/* Called when leaving a ROM list, so the next visit can return to this entry. */
static void menu_remember_rom(void)
{
    if (g_menu.mode != MENU_ROMS)
        return;
    int sys = g_menu.current_system;
    if (sys < 0 || sys >= MAX_TRACKED_SYSTEMS)
        return;
    if (g_menu.count <= 0 || g_menu.selected < 0 || g_menu.selected >= g_menu.count)
        return;
    strncpy(g_last_rom[sys], g_menu.entries[g_menu.selected].path, MAX_NAME - 1);
    g_last_rom[sys][MAX_NAME - 1] = '\0';
}

/*
 * A game can also be a FOLDER holding a multi-file disc image. GDI dumps (the
 * usual Dreamcast format) have to be laid out this way: every GDI game names
 * its tracks track01.bin, track02.raw, ... so two games can never share a
 * folder. Multi-disc PSX sets (.m3u + .cue/.bin per disc) are the same idea.
 *
 * Returns the file inside `dir` that should be launched, picking the most
 * specific descriptor present: m3u (a playlist of discs) > gdi > cue > chd >
 * cdi. Only one level deep. Returns 0 if the folder holds none of them.
 */
static int find_disc_in_dir(const char *dir, char *out, size_t outlen)
{
    static const char *prefer[] = { ".m3u", ".gdi", ".cue", ".chd", ".cdi" };
    char best[MAX_NAME] = "";
    int best_rank = (int)(sizeof(prefer) / sizeof(prefer[0]));

    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        const char *dot = strrchr(de->d_name, '.');
        if (!dot) continue;
        for (int r = 0; r < best_rank; r++) {
            if (strcasecmp(dot, prefer[r]) == 0) {
                best_rank = r;
                strncpy(best, de->d_name, sizeof(best) - 1);
                best[sizeof(best) - 1] = '\0';
                break;
            }
        }
    }
    closedir(d);
    if (!best[0]) return 0;
    snprintf(out, outlen, "%s/%s", dir, best);
    return 1;
}

static void menu_scan_roms(int system_entry_idx)
{
    /* Save system info before overwriting entries */
    int sys_idx = g_menu.entries[system_entry_idx].system_idx;
    char sys_path[MAX_NAME];
    strncpy(sys_path, g_menu.entries[system_entry_idx].path, MAX_NAME - 1);
    sys_path[MAX_NAME - 1] = '\0';

    g_menu.current_system = sys_idx;
    strncpy(g_menu.system_display_name,
            g_menu.entries[system_entry_idx].name, MAX_NAME - 1);
    g_menu.system_display_name[MAX_NAME - 1] = '\0';

    g_menu.mode = MENU_ROMS;
    g_menu.count = 0;
    g_menu.selected = 0;
    g_menu.scroll_top = 0;

    /* Load game name mappings for this system */
    gamenames_load(sys_path);

    DIR *d = opendir(sys_path);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_menu.count < MAX_ENTRIES) {
        if (de->d_name[0] == '.') continue;
        if (de->d_type == DT_DIR) {
            /* A folder is a game if it holds a disc descriptor; listed
             * under the folder's name. See find_disc_in_dir(). */
            char sub[MAX_NAME], disc[MAX_NAME];
            snprintf(sub, sizeof(sub), "%s/%s", sys_path, de->d_name);
            if (!find_disc_in_dir(sub, disc, sizeof(disc)))
                continue;
            MenuEntry *e = &g_menu.entries[g_menu.count++];
            const char *fullname = gamenames_lookup(de->d_name);
            if (fullname)
                strncpy(e->name, fullname, MAX_NAME - 1);
            else
                clean_display_name(de->d_name, e->name, MAX_NAME);
            strncpy(e->path, disc, MAX_NAME - 1);
            e->path[MAX_NAME - 1] = '\0';
            e->system_idx = sys_idx;
            e->rom_count = 0;
            e->is_favorite = favorites_contains(e->path);
            continue;
        }
        if (strcmp(de->d_name, "gamenames.txt") == 0) continue;

        MenuEntry *e = &g_menu.entries[g_menu.count++];
        const char *fullname = gamenames_lookup(de->d_name);
        if (fullname)
            strncpy(e->name, fullname, MAX_NAME - 1);
        else
            clean_display_name(de->d_name, e->name, MAX_NAME);
        snprintf(e->path, MAX_NAME, "%s/%s", sys_path, de->d_name);
        e->system_idx = sys_idx;
        e->rom_count = 0;
        e->is_favorite = favorites_contains(e->path);
    }
    closedir(d);

    /* Hide .bin files when a matching .cue exists (cue+bin = show cue only) */
    for (int i = 0; i < g_menu.count; i++) {
        const char *path_i = g_menu.entries[i].path;
        const char *dot_i = strrchr(path_i, '.');
        if (!dot_i || strcasecmp(dot_i, ".bin") != 0) continue;
        /* Build what the .cue path would look like */
        char cue_path[MAX_NAME];
        int base_len = (int)(dot_i - path_i);
        snprintf(cue_path, MAX_NAME, "%.*s.cue", base_len, path_i);
        /* Check if that .cue exists in our list */
        for (int j = 0; j < g_menu.count; j++) {
            if (strcasecmp(g_menu.entries[j].path, cue_path) == 0) {
                /* Remove .bin entry by shifting the rest down */
                memmove(&g_menu.entries[i], &g_menu.entries[i + 1],
                        (g_menu.count - i - 1) * sizeof(MenuEntry));
                g_menu.count--;
                i--; /* Re-check this index */
                break;
            }
        }
    }

    /* A flat folder holding a .gdi: its .bin/.raw files are that disc's
     * tracks, not games. (Only one GDI set can live in a folder, because the
     * track names collide; more than one needs a subfolder each.) PSX folders
     * never contain a .gdi, so their standalone .bin games are unaffected. */
    bool has_gdi = false;
    for (int i = 0; i < g_menu.count && !has_gdi; i++) {
        const char *dot = strrchr(g_menu.entries[i].path, '.');
        has_gdi = dot && strcasecmp(dot, ".gdi") == 0 &&
                  strncmp(g_menu.entries[i].path, sys_path, strlen(sys_path)) == 0 &&
                  !strchr(g_menu.entries[i].path + strlen(sys_path) + 1, '/');
    }
    for (int i = 0; has_gdi && i < g_menu.count; i++) {
        const char *p = g_menu.entries[i].path;
        const char *dot = strrchr(p, '.');
        if (strchr(p + strlen(sys_path) + 1, '/'))
            continue;           /* entries from subfolders are never tracks */
        if (dot && (strcasecmp(dot, ".bin") == 0 || strcasecmp(dot, ".raw") == 0)) {
            memmove(&g_menu.entries[i], &g_menu.entries[i + 1],
                    (g_menu.count - i - 1) * sizeof(MenuEntry));
            g_menu.count--;
            i--;
        }
    }

    qsort(g_menu.entries, g_menu.count, sizeof(MenuEntry), entry_compare);
    if (g_menu.count >= MAX_ENTRIES)
        printf("MENU: WARNING: hit the %d entry limit in %s; some ROMs are not listed\n",
               MAX_ENTRIES, g_menu.system_display_name);
    printf("MENU: found %d ROMs in %s\n", g_menu.count, g_menu.system_display_name);

    /* Return to whatever was last selected in this system, if it still exists.
     * After the sort, so the index refers to the list the user actually sees.
     * scroll_top is clamped by ui_draw(). */
    if (sys_idx >= 0 && sys_idx < MAX_TRACKED_SYSTEMS && g_last_rom[sys_idx][0]) {
        for (int i = 0; i < g_menu.count; i++) {
            if (strcmp(g_menu.entries[i].path, g_last_rom[sys_idx]) == 0) {
                g_menu.selected = i;
                break;
            }
        }
    }
}

/* =========================================================================
 * Favorites
 * ========================================================================= */

static void ensure_data_dir(void)
{
    mkdir(DATA_DIR, 0755);
    mkdir(STATES_DIR, 0755);
    mkdir(DATA_DIR "/saves", 0755);
}

static void favorites_load(void)
{
    g_favorites_count = 0;
    FILE *f = fopen(FAVORITES_FILE, "r");
    if (!f) return;
    char line[MAX_NAME];
    while (fgets(line, sizeof(line), f) && g_favorites_count < MAX_FAVORITES) {
        /* Strip newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '\0') continue;
        strncpy(g_favorites[g_favorites_count], line, MAX_NAME - 1);
        g_favorites[g_favorites_count][MAX_NAME - 1] = '\0';
        g_favorites_count++;
    }
    fclose(f);
    printf("FAV: loaded %d favorites\n", g_favorites_count);
}

static void favorites_save(void)
{
    ensure_data_dir();
    FILE *f = fopen(FAVORITES_FILE, "w");
    if (!f) { fprintf(stderr, "FAV: cannot write %s\n", FAVORITES_FILE); return; }
    for (int i = 0; i < g_favorites_count; i++)
        fprintf(f, "%s\n", g_favorites[i]);
    fclose(f);
}

static bool favorites_contains(const char *path)
{
    for (int i = 0; i < g_favorites_count; i++)
        if (strcmp(g_favorites[i], path) == 0) return true;
    return false;
}

static void favorites_toggle(const char *path)
{
    /* Remove if exists */
    for (int i = 0; i < g_favorites_count; i++) {
        if (strcmp(g_favorites[i], path) == 0) {
            memmove(&g_favorites[i], &g_favorites[i + 1],
                    (g_favorites_count - i - 1) * sizeof(g_favorites[0]));
            g_favorites_count--;
            favorites_save();
            printf("FAV: removed %s\n", path);
            return;
        }
    }
    /* Add */
    if (g_favorites_count < MAX_FAVORITES) {
        strncpy(g_favorites[g_favorites_count], path, MAX_NAME - 1);
        g_favorites[g_favorites_count][MAX_NAME - 1] = '\0';
        g_favorites_count++;
        favorites_save();
        printf("FAV: added %s\n", path);
    }
}

/* Build a menu from the favorites list */
/*
 * Derive the system index from a ROM path.
 *
 * Recents and Favorites store bare paths, so unlike the browser (which already
 * knows which system directory it is in) they must work the system out again.
 * Both used to do it with:
 *
 *     if (strstr(path, g_systems[s].dir_name))
 *
 * which is case-SENSITIVE, and the ROM partition is vfat mounted
 * shortname=mixed, so directory case is whatever the user created. The N64
 * folder is /opt/roms/N64 while the table says "n64" -- no match, system_idx
 * stayed -1, and the launch path is guarded by `if (e->system_idx >= 0)`, so
 * pressing A on an N64 game in Recents did NOTHING AT ALL. Silently. Atari and
 * MAME worked purely because those folders happen to be lowercase.
 *
 * The directory scanner already had this right (strcasecmp against d_name),
 * which is exactly why browsing worked and Recents did not.
 *
 * Matching the PATH COMPONENT rather than any substring also removes a second
 * hazard: "gb" is a substring of plenty of paths, and first-match-wins would
 * happily assign the wrong core.
 */
static int system_from_path(const char *path)
{
    const char *p = path;
    const char *slash;
    char dir[MAX_NAME];
    size_t n, bl = strlen(ROMS_PATH);

    if (strncmp(path, ROMS_PATH, bl) == 0) {
        p = path + bl;
        while (*p == '/')
            p++;
    } else {
        return -1;                      /* not under the ROM tree */
    }

    slash = strchr(p, '/');
    if (!slash)
        return -1;                      /* file sitting directly in /opt/roms */
    n = (size_t)(slash - p);
    if (n >= sizeof dir)
        n = sizeof dir - 1;
    memcpy(dir, p, n);
    dir[n] = '\0';

    for (int s = 0; g_systems[s].dir_name; s++)
        if (strcasecmp(dir, g_systems[s].dir_name) == 0)
            return s;
    return -1;
}

static void menu_load_favorites(void)
{
    g_menu.mode = MENU_FAVORITES;
    g_menu.count = 0;
    g_menu.selected = 0;
    g_menu.scroll_top = 0;
    strncpy(g_menu.system_display_name, "Favorites", MAX_NAME - 1);

    for (int i = 0; i < g_favorites_count && g_menu.count < MAX_ENTRIES; i++) {
        const char *path = g_favorites[i];
        /* Check that file still exists */
        if (access(path, F_OK) != 0) continue;

        MenuEntry *e = &g_menu.entries[g_menu.count];
        strncpy(e->path, path, MAX_NAME - 1);
        e->path[MAX_NAME - 1] = '\0';
        e->is_favorite = true;

        /* Extract display name from path (use gamenames if available) */
        const char *gname = gamenames_lookup_path(path);
        if (gname) {
            strncpy(e->name, gname, MAX_NAME - 1);
        } else {
            const char *slash = strrchr(path, '/');
            const char *fname = slash ? slash + 1 : path;
            clean_display_name(fname, e->name, MAX_NAME);
        }

        /* Find system index from path */
        e->system_idx = system_from_path(path);

        /* Append system name in parentheses */
        if (e->system_idx >= 0) {
            char tmp[MAX_NAME];
            snprintf(tmp, MAX_NAME, "%s (%s)", e->name, g_systems[e->system_idx].display_name);
            strncpy(e->name, tmp, MAX_NAME - 1);
            e->name[MAX_NAME - 1] = '\0';
        }

        e->rom_count = 0;
        g_menu.count++;
    }
    printf("FAV: showing %d favorites\n", g_menu.count);
}

/* =========================================================================
 * Recently Played
 * ========================================================================= */

static void recents_load(void)
{
    g_recents_count = 0;
    FILE *f = fopen(RECENTS_FILE, "r");
    if (!f) return;
    char line[MAX_NAME];
    while (fgets(line, sizeof(line), f) && g_recents_count < MAX_RECENTS) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '\0') continue;
        strncpy(g_recents[g_recents_count], line, MAX_NAME - 1);
        g_recents[g_recents_count][MAX_NAME - 1] = '\0';
        g_recents_count++;
    }
    fclose(f);
    printf("RECENT: loaded %d recents\n", g_recents_count);
}

static void recents_save(void)
{
    ensure_data_dir();
    FILE *f = fopen(RECENTS_FILE, "w");
    if (!f) return;
    for (int i = 0; i < g_recents_count; i++)
        fprintf(f, "%s\n", g_recents[i]);
    fclose(f);
}

static void recents_add(const char *path)
{
    /* Remove if already present (move to top) */
    for (int i = 0; i < g_recents_count; i++) {
        if (strcmp(g_recents[i], path) == 0) {
            memmove(&g_recents[i], &g_recents[i + 1],
                    (g_recents_count - i - 1) * sizeof(g_recents[0]));
            g_recents_count--;
            break;
        }
    }
    /* Shift everything down and insert at top */
    if (g_recents_count >= MAX_RECENTS) g_recents_count = MAX_RECENTS - 1;
    memmove(&g_recents[1], &g_recents[0], g_recents_count * sizeof(g_recents[0]));
    strncpy(g_recents[0], path, MAX_NAME - 1);
    g_recents[0][MAX_NAME - 1] = '\0';
    g_recents_count++;
    recents_save();
}

/* Build a menu from the recents list */
static void menu_load_recents(void)
{
    g_menu.mode = MENU_RECENTS;
    g_menu.count = 0;
    g_menu.selected = 0;
    g_menu.scroll_top = 0;
    strncpy(g_menu.system_display_name, "Recently Played", MAX_NAME - 1);

    for (int i = 0; i < g_recents_count && g_menu.count < MAX_ENTRIES; i++) {
        const char *path = g_recents[i];
        if (access(path, F_OK) != 0) continue;

        MenuEntry *e = &g_menu.entries[g_menu.count];
        strncpy(e->path, path, MAX_NAME - 1);
        e->path[MAX_NAME - 1] = '\0';
        e->is_favorite = favorites_contains(path);

        const char *gname_r = gamenames_lookup_path(path);
        if (gname_r) {
            strncpy(e->name, gname_r, MAX_NAME - 1);
        } else {
            const char *slash = strrchr(path, '/');
            const char *fname = slash ? slash + 1 : path;
            clean_display_name(fname, e->name, MAX_NAME);
        }

        e->system_idx = system_from_path(path);

        /* Append system name in parentheses */
        if (e->system_idx >= 0) {
            char tmp[MAX_NAME];
            snprintf(tmp, MAX_NAME, "%s (%s)", e->name, g_systems[e->system_idx].display_name);
            strncpy(e->name, tmp, MAX_NAME - 1);
            e->name[MAX_NAME - 1] = '\0';
        }

        e->rom_count = 0;
        g_menu.count++;
    }
    printf("RECENT: showing %d recents\n", g_menu.count);
}

/* =========================================================================
 * State Persistence (remember position across restarts / game exits)
 * ========================================================================= */

static void volume_apply(int pct);
static int volume_query(void);

static void state_save(void)
{
    ensure_data_dir();
    FILE *f = fopen(STATE_FILE, "w");
    if (!f) return;
    fprintf(f, "mode=%d\n", (int)g_menu.mode);
    fprintf(f, "system=%d\n", g_menu.current_system);
    fprintf(f, "selected=%d\n", g_menu.selected);
    fprintf(f, "scroll=%d\n", g_menu.scroll_top);
    fprintf(f, "theme=%d\n", g_current_theme);
    /* Mixer, not g_volume -- volumed may have changed it in-game. */
    fprintf(f, "volume=%d\n", volume_query());
    fclose(f);
}

static void state_load(void)
{
    FILE *f = fopen(STATE_FILE, "r");
    if (!f) {
        /* First boot, no saved state: still push the default to the mixer, so
         * what Settings shows and what the hardware is doing agree. */
        volume_apply(g_volume);
        return;
    }
    int mode = 0, sys = 0, sel = 0, scroll = 0, theme = 0, vol = 80;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        sscanf(line, "mode=%d", &mode);
        sscanf(line, "system=%d", &sys);
        sscanf(line, "selected=%d", &sel);
        sscanf(line, "scroll=%d", &scroll);
        sscanf(line, "theme=%d", &theme);
        sscanf(line, "volume=%d", &vol);
    }
    fclose(f);

    /* Apply theme */
    theme_apply(theme);

    /* Apply volume */
    volume_apply(vol);

    /* Restore menu position */
    if (mode == MENU_SYSTEMS || mode == MENU_FAVORITES || mode == MENU_RECENTS) {
        /* Restore selection in current view */
        if (sel < g_menu.count) g_menu.selected = sel;
        if (scroll < g_menu.count) g_menu.scroll_top = scroll;
    }
    /* Note: we don't auto-restore into ROM browser on cold start;
       after game exit, the launcher restores to system list at the
       system that was just played */
}

/* Count ROMs in a system directory (for display in system list) */
static int count_roms_in_dir(const char *dir_path)
{
    DIR *d = opendir(dir_path);
    if (!d) return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (de->d_type != DT_DIR) {
            count++;
        } else {
            /* A game folder (e.g. a Dreamcast GDI set) counts as one game,
             * matching what menu_scan_roms() lists. Without this, a system
             * whose games are all in folders counted 0 and was hidden. */
            char sub[MAX_NAME], disc[MAX_NAME];
            snprintf(sub, sizeof(sub), "%s/%s", dir_path, de->d_name);
            if (find_disc_in_dir(sub, disc, sizeof(disc)))
                count++;
        }
    }
    closedir(d);
    return count;
}

/* =========================================================================
 * Jump to letter
 *
 * With 663 Atari 2600 ROMs in one folder, stepping the d-pad is not navigation.
 * Three tiers now: UP/DOWN one entry, L1/R1 one page, LEFT/RIGHT one letter.
 * ========================================================================= */

/*
 * Initial letter of an entry, normalised for grouping.
 *
 * The list is sorted with strcasecmp, so case-insensitive grouping matches that
 * order exactly. Anything that is not a letter -- digits, brackets, symbols --
 * collapses into one '#' group, which is where a case-insensitive sort puts
 * them anyway. Without that collapse, LEFT/RIGHT would step one-at-a-time
 * through "1942", "3-D Tic-Tac-Toe", "720 Degrees" instead of clearing the
 * whole numeric run in a single press.
 */
static char menu_entry_initial(int idx)
{
    if (idx < 0 || idx >= g_menu.count)
        return '\0';
    unsigned char c = (unsigned char)g_menu.entries[idx].name[0];
    return isalpha(c) ? (char)toupper(c) : '#';
}

/*
 * Move the selection to the first entry of the previous/next letter group.
 *
 * Deliberately strict: LEFT always lands on the first entry of the PREVIOUS
 * group, never "the top of the current group first". That two-stage behaviour
 * is right for skipping tracks but wrong in a list -- the same button would do
 * different things depending on where the cursor happened to be sitting.
 *
 * scroll_top is not touched: ui_draw() already clamps it so the selection stays
 * visible, and duplicating that here is how the two drift apart.
 */
/*
 * Transient overlay showing the letter just jumped to.
 *
 * Counted in frames rather than wall time: the main loop is a fixed ~30 Hz
 * usleep, so 30 frames is a second, and a frame counter cannot drift out of
 * step with the redraws the way a clock read in an event-driven loop can.
 *
 * The loop only calls ui_draw() when something is dirty, so the countdown has
 * to mark the frame it reaches zero as dirty -- otherwise the overlay would
 * simply stay on screen until the next unrelated redraw.
 */
#define JUMP_HINT_FRAMES 30

static int  g_jump_hint_frames = 0;
static char g_jump_hint_char   = 0;

static bool menu_jump_letter(int dir)
{
    if (g_menu.count <= 0)
        return false;

    char cur = menu_entry_initial(g_menu.selected);
    int  i   = g_menu.selected;

    if (dir > 0) {
        while (i < g_menu.count && menu_entry_initial(i) == cur)
            i++;
        if (i >= g_menu.count)
            return false;              /* already in the last group */
    } else {
        while (i >= 0 && menu_entry_initial(i) == cur)
            i--;
        if (i < 0)
            return false;              /* already in the first group */
        char prev = menu_entry_initial(i);
        while (i > 0 && menu_entry_initial(i - 1) == prev)
            i--;                       /* rewind to the START of that group */
    }

    g_menu.selected     = i;
    g_jump_hint_char    = menu_entry_initial(i);
    g_jump_hint_frames  = JUMP_HINT_FRAMES;
    return true;
}

/* =========================================================================
 * Settings Menu
 * ========================================================================= */

typedef enum {
    SETTING_THEME,
    SETTING_VOLUME,
    SETTING_N64_QUALITY,
    SETTING_FAVORITES,
    SETTING_RECENTS,
    SETTING_CLEAR_RECENTS,
    SETTING_COUNT
} SettingItem;

static const char *setting_names[] = {
    "Color Theme",
    "Volume",
    "N64 Quality",
    "View Favorites",
    "Recently Played",
    "Clear Recent",
};

/*
 * Volume control.
 *
 * On the A33 this drove the codec's analog 'Headphone Playback Volume'. HDMI
 * has no analog stage, and the H616 audio-hub driver's own controls vary with
 * the kernel patch set, so the launcher does not touch the hardware mixer at all.
 *
 * Instead /etc/asound.conf (generated by S35alsa for whichever card is the
 * HDMI output) puts an ALSA softvol plugin in front of the card, with one
 * control:
 *
 *     'Master Playback Volume'   101 steps, index 0..100
 *                                0 = MUTE, 1..100 = -40 dB .. 0 dB
 *
 * Index == percent by construction, so there is no scale to translate and none
 * for volumed (S45volumed) to disagree with. The 40 dB span is the same
 * usable range the reference project settled on. ctl.!default points at the
 * same card, so plain `amixer` with no -c finds the control.
 *
 * The softvol control only exists once something has opened the "default" PCM.
 * S35alsa does that once at boot, before restoring the saved level.
 */
#define VOL_CONTROL     "'Master Playback Volume'"

static void volume_apply(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_volume = pct;

    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "amixer -q cset name=" VOL_CONTROL " %d >/dev/null 2>&1", pct);
    if (system(cmd) == -1) {
        /* best effort: a failed amixer leaves the level where it was */
    }
}

/*
 * Read the volume back OUT of the mixer.
 *
 * Needed because the launcher is not the only thing that moves this control:
 * volumed (S45volumed) handles PS + d-pad while a GAME is running, where the
 * launcher's Settings menu cannot be reached. Persisting g_volume in that case would
 * write back a stale in-memory value and silently undo what the player just
 * set. So the mixer is the single source of truth at save time.
 */
static int volume_query(void)
{
    FILE *p = popen("amixer cget name=" VOL_CONTROL " 2>/dev/null"
                    " | sed -n 's/.*: values=\\([0-9]*\\).*/\\1/p' | head -1", "r");
    int pct = -1;

    if (!p) return g_volume;
    if (fscanf(p, "%d", &pct) != 1) pct = -1;
    pclose(p);
    if (pct < 0) return g_volume;          /* unreadable: keep what we had */
    if (pct > 100) pct = 100;
    return pct;
}

/*
 * N64 quality toggle: parallel-n64 at 320x240 or 640x480.
 *
 * On the A33 this also raised the Mali-400's clock, because that GPU was
 * geometry-bound (see the reference DevelopmentLog). The Mali-G31 here has no
 * such hack. The toggle is just the resolution, and which one is the right
 * default on this GPU has yet to be measured.
 *
 * NO SEPARATE COPY OF THIS STATE IS KEPT. The .opt file is the single source of
 * truth and is read back each time. Keeping a duplicate in state.txt is what
 * made the volume setting silently revert on the reference board -- two
 * writers, two scales, one control.
 */
static int n64_hires_get(void)
{
    FILE *p = popen("grep -c \"^parallel-n64-screensize = \\\"640x480\\\"\" '/root/.config/retroarch/config/ParaLLEl N64/ParaLLEl N64.opt' 2>/dev/null", "r");
    int n = 0;

    if (!p) return 0;
    if (fscanf(p, "%d", &n) != 1) n = 0;
    pclose(p);
    return n > 0;
}

static void n64_hires_set(int on)
{
    if (system(on ? "/usr/sbin/n64-hires on >/dev/null 2>&1"
                  : "/usr/sbin/n64-hires off >/dev/null 2>&1") == -1) {
        /* best effort: the Settings value is re-read from the .opt anyway */
    }
}

static void menu_load_settings(void)
{
    g_menu.mode = MENU_SETTINGS;
    g_menu.count = SETTING_COUNT;
    g_menu.selected = 0;
    g_menu.scroll_top = 0;
    strncpy(g_menu.system_display_name, "Settings", MAX_NAME - 1);

    for (int i = 0; i < SETTING_COUNT && i < MAX_ENTRIES; i++) {
        MenuEntry *e = &g_menu.entries[i];
        strncpy(e->name, setting_names[i], MAX_NAME - 1);
        e->path[0] = '\0';
        e->system_idx = -1;
        e->rom_count = 0;
        e->is_favorite = false;
    }
}

/* =========================================================================
 * UI Rendering
 * ========================================================================= */

static void ui_draw_header(const char *title)
{
    gfx_fill_rect(0, 0, SCREEN_WIDTH, HEADER_HEIGHT, COL_HEADER_BG);

    if (g_menu.mode == MENU_SYSTEMS) {
        gfx_draw_text(g_font_header, "RETRO", PADDING_X, UI_SCALE(8), COL_SYSTEM_ICON, 0);
        int w = 0;
        TTF_SizeUTF8(g_font_header, "RETRO", &w, NULL);
        gfx_draw_text(g_font_header, " LAUNCHER", PADDING_X + w, UI_SCALE(8), COL_TEXT, 0);
    } else if (g_menu.mode == MENU_SETTINGS) {
        gfx_draw_text(g_font_header, "\xe2\x97\x80", PADDING_X, UI_SCALE(8), COL_TEXT_DIM, 0);
        gfx_draw_text(g_font_header, "Settings", PADDING_X + UI_SCALE(28), UI_SCALE(8), COL_TEXT, 0);

        /* Show current theme name on the right */
        const char *tname = g_themes[g_current_theme].name;
        int tw;
        TTF_SizeUTF8(g_font_small, tname, &tw, NULL);
        gfx_draw_text(g_font_small, tname, SCREEN_WIDTH - PADDING_X - tw, UI_SCALE(14),
                      COL_SYSTEM_ICON, 0);
    } else {
        /* Back arrow + system name */
        gfx_draw_text(g_font_header, "\xe2\x97\x80", PADDING_X, UI_SCALE(8), COL_TEXT_DIM, 0);
        gfx_draw_text(g_font_header, title, PADDING_X + UI_SCALE(28), UI_SCALE(8), COL_TEXT, 0);
    }

    /* ROM count on the right (except settings) */
    if (g_menu.mode != MENU_SETTINGS) {
        char count_str[32];
        snprintf(count_str, sizeof(count_str), "%d", g_menu.count);
        int tw;
        TTF_SizeUTF8(g_font_small, count_str, &tw, NULL);
        gfx_draw_text(g_font_small, count_str, SCREEN_WIDTH - PADDING_X - tw, UI_SCALE(14),
                      COL_TEXT_DIM, 0);
    }
}

static void ui_draw_footer(void)
{
    gfx_fill_rect(0, SCREEN_HEIGHT - FOOTER_HEIGHT, SCREEN_WIDTH, FOOTER_HEIGHT,
                  COL_FOOTER_BG);

    int x = PADDING_X;
    int y = SCREEN_HEIGHT - FOOTER_HEIGHT + UI_SCALE(8);

    if (g_menu.mode == MENU_SYSTEMS) {
        /* A=Open */
        gfx_fill_rect(x, y, UI_SCALE(20), UI_SCALE(20), COL_HIGHLIGHT);
        gfx_draw_text(g_font_small, "A", x + UI_SCALE(5), y + UI_SCALE(2), COL_TEXT, 0);
        x += UI_SCALE(26);
        gfx_draw_text(g_font_small, "OPEN", x, y + UI_SCALE(2), COL_TEXT_DIM, 0);
    } else if (g_menu.mode == MENU_SETTINGS) {
        /* A=Select  B=Back */
        gfx_fill_rect(x, y, UI_SCALE(20), UI_SCALE(20), COL_HIGHLIGHT);
        gfx_draw_text(g_font_small, "A", x + UI_SCALE(5), y + UI_SCALE(2), COL_TEXT, 0);
        x += UI_SCALE(26);
        x += gfx_draw_text(g_font_small, "SELECT", x, y + UI_SCALE(2), COL_TEXT_DIM, 0);
        x += UI_SCALE(20);
        gfx_fill_rect(x, y, UI_SCALE(20), UI_SCALE(20), COL_HEADER_BG);
        gfx_draw_text(g_font_small, "B", x + UI_SCALE(5), y + UI_SCALE(2), COL_TEXT, 0);
        x += UI_SCALE(26);
        gfx_draw_text(g_font_small, "BACK", x, y + UI_SCALE(2), COL_TEXT_DIM, 0);
    } else {
        /* A=Play  B=Back  X=Fav */
        gfx_fill_rect(x, y, UI_SCALE(20), UI_SCALE(20), COL_HIGHLIGHT);
        gfx_draw_text(g_font_small, "A", x + UI_SCALE(5), y + UI_SCALE(2), COL_TEXT, 0);
        x += UI_SCALE(26);
        x += gfx_draw_text(g_font_small, "PLAY", x, y + UI_SCALE(2), COL_TEXT_DIM, 0);
        x += UI_SCALE(20);
        gfx_fill_rect(x, y, UI_SCALE(20), UI_SCALE(20), COL_HEADER_BG);
        gfx_draw_text(g_font_small, "B", x + UI_SCALE(5), y + UI_SCALE(2), COL_TEXT, 0);
        x += UI_SCALE(26);
        x += gfx_draw_text(g_font_small, "BACK", x, y + UI_SCALE(2), COL_TEXT_DIM, 0);
        x += UI_SCALE(20);
        gfx_fill_rect(x, y, UI_SCALE(20), UI_SCALE(20), COL_STAR);
        gfx_draw_text(g_font_small, "\xe2\x96\xb3", x + UI_SCALE(3), y + UI_SCALE(2), COL_HEADER_BG, 0);
        x += UI_SCALE(26);
        x += gfx_draw_text(g_font_small, "FAV", x, y + UI_SCALE(2), COL_TEXT_DIM, 0);
        x += UI_SCALE(20);
        gfx_fill_rect(x, y, UI_SCALE(20), UI_SCALE(20), COL_HEADER_BG);
        gfx_draw_text(g_font_small, "\xe2\x96\xa1", x + UI_SCALE(3), y + UI_SCALE(2), COL_TEXT, 0);
        x += UI_SCALE(26);
        gfx_draw_text(g_font_small, "SRC", x, y + UI_SCALE(2), COL_TEXT_DIM, 0);
    }

    /* Page indicator on right side (for all list modes) */
    if (g_menu.count > MAX_VISIBLE) {
        int page = (g_menu.selected / MAX_VISIBLE) + 1;
        int total_pages = (g_menu.count + MAX_VISIBLE - 1) / MAX_VISIBLE;
        char scroll[32];
        snprintf(scroll, sizeof(scroll), "%d/%d  pg %d/%d",
                 g_menu.selected + 1, g_menu.count, page, total_pages);
        int tw;
        TTF_SizeUTF8(g_font_small, scroll, &tw, NULL);
        gfx_draw_text(g_font_small, scroll,
                      SCREEN_WIDTH - PADDING_X - tw,
                      y + UI_SCALE(2), COL_TEXT_DIM, 0);
    }
}

static void ui_draw_list(void)
{
    int visible = MAX_VISIBLE;
    if (g_menu.count < visible) visible = g_menu.count;

    /* Adjust scroll position */
    if (g_menu.selected < g_menu.scroll_top)
        g_menu.scroll_top = g_menu.selected;
    if (g_menu.selected >= g_menu.scroll_top + MAX_VISIBLE)
        g_menu.scroll_top = g_menu.selected - MAX_VISIBLE + 1;

    for (int i = 0; i < visible && (g_menu.scroll_top + i) < g_menu.count; i++) {
        int idx = g_menu.scroll_top + i;
        MenuEntry *e = &g_menu.entries[idx];
        int y = LIST_TOP + i * ITEM_HEIGHT;
        int text_y = y + (ITEM_HEIGHT - FONT_SIZE_LIST) / 2;
        int text_max_w = SCREEN_WIDTH - 2 * PADDING_X - UI_SCALE(8);

        /* Reserve space for star icon on ROM entries */
        bool show_star = (g_menu.mode == MENU_ROMS || g_menu.mode == MENU_FAVORITES ||
                          g_menu.mode == MENU_RECENTS) && e->is_favorite;
        if (g_menu.mode == MENU_ROMS || g_menu.mode == MENU_FAVORITES ||
            g_menu.mode == MENU_RECENTS)
            text_max_w -= UI_SCALE(30); /* Space for star */

        if (idx == g_menu.selected) {
            /* Highlighted selection pill */
            gfx_draw_pill(PADDING_X - UI_SCALE(8), y + UI_SCALE(2), SCREEN_WIDTH - 2 * PADDING_X + UI_SCALE(16),
                          ITEM_HEIGHT - UI_SCALE(4), COL_HIGHLIGHT);
            gfx_draw_text(g_font_list, e->name, PADDING_X + UI_SCALE(4), text_y,
                          COL_TEXT, text_max_w);
        } else {
            gfx_draw_text(g_font_list, e->name, PADDING_X + UI_SCALE(4), text_y,
                          COL_TEXT, text_max_w);
        }

        /* Favorite star (filled polygon) */
        if (show_star) {
            gfx_draw_star(SCREEN_WIDTH - PADDING_X - UI_SCALE(12),
                          text_y + FONT_SIZE_LIST / 2 + 1,
                          UI_SCALE(9), UI_SCALE(4), COL_STAR);
        }

        /* Settings mode: show current value on the right */
        if (g_menu.mode == MENU_SETTINGS) {
            const char *val_str = NULL;
            char val_buf[32];
            if (idx == SETTING_THEME) {
                val_str = g_themes[g_current_theme].name;
            } else if (idx == SETTING_VOLUME) {
                snprintf(val_buf, sizeof(val_buf), "%d%%", g_volume);
                val_str = val_buf;
            } else if (idx == SETTING_N64_QUALITY) {
                val_str = n64_hires_get() ? "640x480" : "320x240";
            } else if (idx == SETTING_CLEAR_RECENTS) {
                snprintf(val_buf, sizeof(val_buf), "%d items", g_recents_count);
                val_str = val_buf;
            }
            if (val_str) {
                int tw;
                TTF_SizeUTF8(g_font_small, val_str, &tw, NULL);
                gfx_draw_text(g_font_small, val_str,
                              SCREEN_WIDTH - PADDING_X - tw - UI_SCALE(8), text_y + UI_SCALE(3),
                              COL_SYSTEM_ICON, 0);
            }
        }

        /* Divider line */
        if (idx != g_menu.selected && (idx + 1) != g_menu.selected) {
            gfx_fill_rect(PADDING_X, y + ITEM_HEIGHT - 1,
                          SCREEN_WIDTH - 2 * PADDING_X, 1, COL_DIVIDER);
        }
    }

    /* Empty state */
    if (g_menu.count == 0) {
        gfx_draw_text_centered(g_font_list, "No items found",
                                SCREEN_HEIGHT / 2 - UI_SCALE(12), COL_TEXT_DIM);
    }

    /* Scrollbar */
    if (g_menu.count > MAX_VISIBLE) {
        int track_h = LIST_BOTTOM - LIST_TOP;
        int thumb_h = (visible * track_h) / g_menu.count;
        if (thumb_h < 10) thumb_h = 10;
        int thumb_y = LIST_TOP + (g_menu.scroll_top * (track_h - thumb_h))
                      / (g_menu.count - MAX_VISIBLE);
        gfx_fill_rect(SCREEN_WIDTH - UI_SCALE(4), LIST_TOP, UI_SCALE(3), track_h, COL_HEADER_BG);
        gfx_fill_rect(SCREEN_WIDTH - UI_SCALE(4), thumb_y, UI_SCALE(3), thumb_h, COL_HIGHLIGHT);
    }
}

/*
 * Big letter shown briefly after a letter jump.
 *
 * Drawn last so it sits above the list, and centred rather than tucked into the
 * header: the point is to be readable without looking away from where the eye
 * already is while holding LEFT/RIGHT. A header-corner indicator was the
 * simpler option but needs to be hunted for, which defeats it.
 */
static void ui_draw_jump_hint(void)
{
    if (g_jump_hint_frames <= 0 || !g_jump_hint_char || !g_font_jump)
        return;

    char s[2] = { g_jump_hint_char, 0 };

    int tw = 0, th = 0;
    TTF_SizeUTF8(g_font_jump, s, &tw, &th);

    int pad = UI_SCALE(24);
    int bw  = tw + pad * 2;
    int bh  = th + pad * 2;
    int bx  = (SCREEN_WIDTH  - bw) / 2;
    int by  = (SCREEN_HEIGHT - bh) / 2;

    /* Plain filled panel: no alpha blending in this renderer, and a solid
     * block reads better at a glance than an outline would. */
    gfx_fill_rect(bx, by, bw, bh, COL_HEADER_BG);
    gfx_draw_text_centered(g_font_jump, s, by + pad, COL_SYSTEM_ICON);
}

static void ui_draw(void)
{
    gfx_clear(COL_BG);

    const char *title;
    switch (g_menu.mode) {
    case MENU_ROMS:      title = g_menu.system_display_name; break;
    case MENU_FAVORITES: title = "Favorites"; break;
    case MENU_RECENTS:   title = "Recently Played"; break;
    case MENU_SETTINGS:  title = "Settings"; break;
    default:             title = "Systems"; break;
    }
    ui_draw_header(title);
    ui_draw_list();
    ui_draw_footer();
    ui_draw_jump_hint();

    drm_flip();
}

/* =========================================================================
 * Save State Slot Detection & Picker
 * ========================================================================= */

/* Map core .so filename to RetroArch's core display name used in state dirs */
static const char *core_display_name(const char *core_file)
{
    /* Table matching g_systems[].core_file -> RetroArch core info name */
    static const struct { const char *so; const char *name; } map[] = {
        { "fceumm_libretro.so",            "FCEUmm" },
        { "snes9x2005_libretro.so",        "Snes9x 2005" },
        { "gambatte_libretro.so",          "Gambatte" },
        { "mgba_libretro.so",              "mGBA" },
        { "genesisplusgx_libretro.so",     "Genesis Plus GX" },
        { "stella2023_libretro.so",        "Stella 2023" },
        { "prosystem_libretro.so",         "ProSystem" },
        { "atari800_libretro.so",          "Atari800" },
        { "beetlepcefast_libretro.so",     "Beetle PCE Fast" },
        { "beetlesupergrafx_libretro.so",  "Beetle SuperGrafx" },
        { "fuse_libretro.so",             "Fuse" },
        { "prboom_libretro.so",           "PrBoom" },
        { "fbalpha2012_libretro.so",      "FB Alpha 2012" },
        { "mame2003plus_libretro.so",     "MAME 2003-Plus" },
        /* Missing until now, so the save-state slot picker never appeared
         * for PSX, N64 or GBA. Names are each core's library_name, which is
         * what RetroArch uses for the per-core state directory. */
        { "pcsx_rearmed_libretro.so",     "PCSX-ReARMed" },
        { "paralleln64_libretro.so",      "ParaLLEl N64" },
        { "gpsp_libretro.so",             "gpSP" },
        { "ppsspp_libretro.so",           "PPSSPP" },
        { "flycast_libretro.so",          "Flycast" },
        { NULL, NULL }
    };
    for (int i = 0; map[i].so; i++) {
        if (strcmp(core_file, map[i].so) == 0)
            return map[i].name;
    }
    return NULL;
}

/* Build the state file path for a given ROM, core, and slot.
 * RetroArch convention: <states_dir>/<CoreName>/<rombasename>.state[N]
 * Slot 0 = ".state", slot 1 = ".state1", etc. */
static void build_state_path(char *out, int out_size,
                             const char *rom_path, const char *core_file,
                             int slot)
{
    const char *cname = core_display_name(core_file);
    if (!cname) { out[0] = '\0'; return; }

    /* Extract ROM basename without extension */
    const char *slash = strrchr(rom_path, '/');
    const char *base = slash ? slash + 1 : rom_path;
    char rombase[256];
    strncpy(rombase, base, sizeof(rombase) - 1);
    rombase[sizeof(rombase) - 1] = '\0';
    char *dot = strrchr(rombase, '.');
    if (dot) *dot = '\0';

    if (slot == 0)
        snprintf(out, out_size, "%s/%s/%s.state", STATES_DIR, cname, rombase);
    else
        snprintf(out, out_size, "%s/%s/%s.state%d", STATES_DIR, cname, rombase, slot);
}

/* Check which slots (0-9) have save state files. Returns bitmask. */
static int detect_save_slots(const char *rom_path, const char *core_file)
{
    int mask = 0;
    char path[MAX_NAME];
    for (int i = 0; i < MAX_SLOTS; i++) {
        build_state_path(path, sizeof(path), rom_path, core_file, i);
        if (path[0] && access(path, F_OK) == 0)
            mask |= (1 << i);
    }
    return mask;
}

/* Interactive save state slot picker.
 * Returns: -1 = cancelled, -2 = new game (no state load), 0-9 = slot to load.
 * Draws a modal overlay on screen. */
static int slot_picker(const char *rom_path, int system_idx)
{
    int slot_mask = detect_save_slots(rom_path, g_systems[system_idx].core_file);

    /* If no save states exist, just launch immediately */
    if (slot_mask == 0)
        return -2;

    /* Count populated slots and build entries:
     * Entry 0 = "New Game" (no state load)
     * Entry 1..N = populated slots */
    int slots[MAX_SLOTS];
    int num_slots = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slot_mask & (1 << i))
            slots[num_slots++] = i;
    }

    int selected = 0;  /* Start on "New Game" */
    int total = num_slots + 1;  /* +1 for "New Game" */

    /* Modal loop */
    for (;;) {
        /* Draw background dim */
        gfx_clear(COL_BG);

        /* Header */
        const char *romslash = strrchr(rom_path, '/');
        const char *romname = romslash ? romslash + 1 : rom_path;
        char title[128];
        snprintf(title, sizeof(title), "Load State: %.80s", romname);
        ui_draw_header(title);

        /* Draw slot list */
        int y = LIST_TOP + UI_SCALE(8);
        int item_h = ITEM_HEIGHT;

        /* "New Game" entry */
        {
            pixel_t bg = (selected == 0) ? COL_HIGHLIGHT : COL_BG;
            pixel_t fg = (selected == 0) ? COL_HIGHLIGHT2 : COL_TEXT;
            gfx_fill_rect(UI_SCALE(20), y, SCREEN_WIDTH - UI_SCALE(40), item_h, bg);
            gfx_draw_text(g_font_list, "New Game (no state)", UI_SCALE(40), y + UI_SCALE(4), fg, 0);
            y += item_h;
        }

        /* Divider */
        gfx_fill_rect(UI_SCALE(30), y, SCREEN_WIDTH - UI_SCALE(60), 1, COL_DIVIDER);
        y += UI_SCALE(4);

        /* Slot entries */
        for (int i = 0; i < num_slots; i++) {
            int idx = i + 1; /* offset by "New Game" entry */
            pixel_t bg = (selected == idx) ? COL_HIGHLIGHT : COL_BG;
            pixel_t fg = (selected == idx) ? COL_HIGHLIGHT2 : COL_TEXT;
            gfx_fill_rect(UI_SCALE(20), y, SCREEN_WIDTH - UI_SCALE(40), item_h, bg);

            /* Show slot number and file info */
            char label[128];
            char spath[MAX_NAME];
            build_state_path(spath, sizeof(spath), rom_path,
                             g_systems[system_idx].core_file, slots[i]);
            struct stat st;
            if (stat(spath, &st) == 0) {
                /* Show file size */
                int kb = (int)(st.st_size / 1024);
                snprintf(label, sizeof(label), "Slot %d  (%d KB)", slots[i], kb);
            } else {
                snprintf(label, sizeof(label), "Slot %d", slots[i]);
            }
            gfx_draw_text(g_font_list, label, UI_SCALE(40), y + UI_SCALE(4), fg, 0);
            y += item_h;
        }

        /* Footer hint */
        gfx_fill_rect(0, SCREEN_HEIGHT - FOOTER_HEIGHT, SCREEN_WIDTH,
                       FOOTER_HEIGHT, COL_FOOTER_BG);
        gfx_draw_text(g_font_small, "\xE2\x9C\x95:Select  O:Cancel",
                       UI_SCALE(20), SCREEN_HEIGHT - FOOTER_HEIGHT + UI_SCALE(8), COL_TEXT_DIM, 0);

        drm_flip();

        /* Input */
        input_poll();

        if (input_just_pressed_or_repeat(BTN_UP_MASK)) {
            if (selected > 0) selected--;
        }
        if (input_just_pressed_or_repeat(BTN_DOWN_MASK)) {
            if (selected < total - 1) selected++;
        }
        if (g_input.pressed & BTN_A_MASK) {
            if (selected == 0)
                return -2;  /* New Game */
            else
                return slots[selected - 1];  /* Slot number */
        }
        if (g_input.pressed & BTN_B_MASK) {
            return -1;  /* Cancel */
        }
    }
}

/* =========================================================================
 * Search Keyboard
 * ========================================================================= */

/* Case-insensitive substring match */
static bool str_icontains(const char *hay, const char *needle)
{
    if (!needle[0]) return true;
    int nlen = strlen(needle);
    int hlen = strlen(hay);
    if (nlen > hlen) return false;
    for (int i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (int j = 0; j < nlen; j++) {
            if (tolower((unsigned char)hay[i + j]) !=
                tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

/* Case-insensitive prefix check */
static bool str_istarts(const char *hay, const char *needle)
{
    for (int i = 0; needle[i]; i++) {
        if (tolower((unsigned char)hay[i]) !=
            tolower((unsigned char)needle[i]))
            return false;
    }
    return true;
}

/* Rebuild filtered index list from search text.
 * Prefix matches come first, then substring-only matches. */
static void search_rebuild(int *filt, int *fcnt, int *fsel, int *fscr,
                            const char *text, int tlen)
{
    *fcnt = 0; *fsel = 0; *fscr = 0;
    if (tlen == 0) {
        for (int i = 0; i < g_menu.count; i++)
            filt[(*fcnt)++] = i;
        return;
    }
    /* Pass 1: prefix matches */
    for (int i = 0; i < g_menu.count; i++) {
        if (str_istarts(g_menu.entries[i].name, text))
            filt[(*fcnt)++] = i;
    }
    /* Pass 2: substring-only matches */
    for (int i = 0; i < g_menu.count; i++) {
        if (!str_istarts(g_menu.entries[i].name, text) &&
            str_icontains(g_menu.entries[i].name, text))
            filt[(*fcnt)++] = i;
    }
}

/* Interactive on-screen keyboard for searching the current list.
 * Returns index into g_menu.entries, or -1 if cancelled. */
static int search_run(void)
{
    char text[49] = "";
    int tlen = 0;

    int kb_r = 1, kb_c = 0;    /* keyboard cursor */
    bool in_list = false;       /* true = navigating list */

    static const char *krows[4] = {
        "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"
    };

    /* Keyboard geometry (comments give the values at 480 lines) */
    enum {
        KW  = UI_SCALE(68),  KH  = UI_SCALE(38),  KG  = UI_SCALE(4),
        KRP = KH + KG,                      /* 42 row pitch */
        KY0 = UI_SCALE(270),                /* first row Y */
        KLT = HEADER_HEIGHT + 4,            /* list top */
        KLB = KY0 - UI_SCALE(4),            /* list bottom */
        KIH = UI_SCALE(36),                 /* list item height */
        KLV = (KLB - KLT) / KIH             /* visible items (~6) */
    };

    int filt[MAX_ENTRIES];
    int fcnt = 0, fsel = 0, fscr = 0;

    search_rebuild(filt, &fcnt, &fsel, &fscr, text, tlen);
    touch_poll(); /* drain stale events */

    for (;;) {
        input_poll();
        touch_poll();

        /* ---- Touch input ---- */
        if (g_touch.just_down) {
            int tx = g_touch.x, ty = g_touch.y;

            /* Tap in list area */
            if (ty >= KLT && ty < KLB && fcnt > 0) {
                int idx = (ty - KLT) / KIH + fscr;
                if (idx < fcnt)
                    return filt[idx];
            }

            /* Tap on keyboard rows 0-3 */
            if (ty >= KY0 && ty < KY0 + 4 * KRP) {
                int row = (ty - KY0) / KRP;
                int nk = (int)strlen(krows[row]);
                int extra = (row == 2) ? 1 : 0;
                int total = nk + extra;
                int rx = (SCREEN_WIDTH - total * (KW + KG) + KG) / 2;
                int col = (tx - rx + KG / 2) / (KW + KG);
                if (col >= 0 && col < nk && tx >= rx) {
                    if (tlen < 48) {
                        text[tlen++] = krows[row][col];
                        text[tlen] = '\0';
                        search_rebuild(filt, &fcnt, &fsel, &fscr, text, tlen);
                    }
                } else if (row == 2 && col == nk && tx >= rx) {
                    if (tlen > 0) {
                        text[--tlen] = '\0';
                        search_rebuild(filt, &fcnt, &fsel, &fscr, text, tlen);
                    }
                }
            }

            /* Tap on row 4 (SPACE / CLR / OK) */
            if (ty >= KY0 + 4 * KRP && ty < KY0 + 5 * KRP) {
                int rx = (SCREEN_WIDTH - 10 * (KW + KG) + KG) / 2;
                int sw = 5 * (KW + KG) - KG;
                int cw = 2 * KW + KG;
                if (tx >= rx && tx < rx + sw) {
                    if (tlen < 48) {
                        text[tlen++] = ' '; text[tlen] = '\0';
                        search_rebuild(filt, &fcnt, &fsel, &fscr, text, tlen);
                    }
                } else if (tx >= rx + sw + KG && tx < rx + sw + KG + cw) {
                    tlen = 0; text[0] = '\0';
                    search_rebuild(filt, &fcnt, &fsel, &fscr, text, tlen);
                } else if (tx >= rx + sw + KG + cw + KG) {
                    return (fcnt > 0) ? filt[fsel] : -1;
                }
            }
        }

        /* ---- Gamepad input ---- */
        if (g_input.pressed & BTN_B_MASK)
            return -1;

        if (in_list) {
            if (input_just_pressed_or_repeat(BTN_DOWN_MASK)) {
                if (fsel < fcnt - 1) fsel++;
                else { in_list = false; kb_r = 0; kb_c = 0; }
            }
            if (input_just_pressed_or_repeat(BTN_UP_MASK)) {
                if (fsel > 0) fsel--;
            }
            if ((g_input.pressed & BTN_A_MASK) && fcnt > 0)
                return filt[fsel];
            if (fsel < fscr) fscr = fsel;
            if (fsel >= fscr + KLV) fscr = fsel - KLV + 1;
        } else {
            int max_c;
            if (kb_r < 4)
                max_c = (int)strlen(krows[kb_r]) - 1 + (kb_r == 2 ? 1 : 0);
            else
                max_c = 2;

            if (input_just_pressed_or_repeat(BTN_RIGHT_MASK) && kb_c < max_c)
                kb_c++;
            if (input_just_pressed_or_repeat(BTN_LEFT_MASK) && kb_c > 0)
                kb_c--;
            if (input_just_pressed_or_repeat(BTN_DOWN_MASK) && kb_r < 4)
                kb_r++;
            if (input_just_pressed_or_repeat(BTN_UP_MASK)) {
                if (kb_r > 0) kb_r--;
                else if (fcnt > 0) { in_list = true; fsel = 0; fscr = 0; }
            }

            /* Clamp column for current row */
            if (kb_r < 4) {
                int mc = (int)strlen(krows[kb_r]) - 1 + (kb_r == 2 ? 1 : 0);
                if (kb_c > mc) kb_c = mc;
            } else if (kb_c > 2) kb_c = 2;

            if (g_input.pressed & BTN_A_MASK) {
                if (kb_r < 4) {
                    int nk = (int)strlen(krows[kb_r]);
                    if (kb_c < nk && tlen < 48) {
                        text[tlen++] = krows[kb_r][kb_c];
                        text[tlen] = '\0';
                        search_rebuild(filt, &fcnt, &fsel, &fscr, text, tlen);
                    } else if (kb_r == 2 && kb_c == nk && tlen > 0) {
                        text[--tlen] = '\0';
                        search_rebuild(filt, &fcnt, &fsel, &fscr, text, tlen);
                    }
                } else {
                    if (kb_c == 0 && tlen < 48) {
                        text[tlen++] = ' '; text[tlen] = '\0';
                        search_rebuild(filt, &fcnt, &fsel, &fscr, text, tlen);
                    } else if (kb_c == 1) {
                        tlen = 0; text[0] = '\0';
                        search_rebuild(filt, &fcnt, &fsel, &fscr, text, tlen);
                    } else if (kb_c == 2) {
                        return (fcnt > 0) ? filt[fsel] : -1;
                    }
                }
            }
        }

        /* ---- Render ---- */
        gfx_clear(COL_BG);

        /* Search bar (header) */
        gfx_fill_rect(0, 0, SCREEN_WIDTH, HEADER_HEIGHT, COL_HEADER_BG);
        {
            char bar[80];
            snprintf(bar, sizeof(bar), "Search: %s_", text);
            gfx_draw_text(g_font_header, bar, PADDING_X, UI_SCALE(8), COL_TEXT,
                          SCREEN_WIDTH - 2 * PADDING_X - UI_SCALE(60));
            char cnt[16];
            snprintf(cnt, sizeof(cnt), "%d", fcnt);
            int tw;
            TTF_SizeUTF8(g_font_small, cnt, &tw, NULL);
            gfx_draw_text(g_font_small, cnt,
                          SCREEN_WIDTH - PADDING_X - tw, UI_SCALE(14), COL_TEXT_DIM, 0);
        }

        /* Filtered list */
        {
            int vis = KLV < fcnt ? KLV : fcnt;
            for (int i = 0; i < vis && (fscr + i) < fcnt; i++) {
                int ei = filt[fscr + i];
                int y = KLT + i * KIH;
                int ty = y + (KIH - FONT_SIZE_LIST) / 2;
                bool sel = (fscr + i == fsel);

                if (sel && in_list)
                    gfx_draw_pill(PADDING_X - UI_SCALE(8), y + UI_SCALE(2),
                                  SCREEN_WIDTH - 2 * PADDING_X + UI_SCALE(16),
                                  KIH - UI_SCALE(4), COL_HIGHLIGHT);

                gfx_draw_text(g_font_list, g_menu.entries[ei].name,
                              PADDING_X + UI_SCALE(4), ty,
                              (sel && in_list) ? COL_TEXT : COL_TEXT_DIM,
                              SCREEN_WIDTH - 2 * PADDING_X);
            }
            if (fcnt == 0 && tlen > 0)
                gfx_draw_text_centered(g_font_small, "No matches",
                                       (KLT + KLB) / 2 - UI_SCALE(8), COL_TEXT_DIM);
        }

        /* Divider between list and keyboard */
        gfx_fill_rect(UI_SCALE(20), KY0 - UI_SCALE(2), SCREEN_WIDTH - UI_SCALE(40), 1, COL_DIVIDER);

        /* Keyboard rows 0-3 */
        for (int r = 0; r < 4; r++) {
            int nk = (int)strlen(krows[r]);
            int extra = (r == 2) ? 1 : 0;
            int total = nk + extra;
            int rx = (SCREEN_WIDTH - total * (KW + KG) + KG) / 2;
            int ry = KY0 + r * KRP;

            for (int c = 0; c < total; c++) {
                int kx = rx + c * (KW + KG);
                bool hl = (!in_list && kb_r == r && kb_c == c);
                pixel_t bg = hl ? COL_HIGHLIGHT : COL_HEADER_BG;
                pixel_t fg = hl ? COL_TEXT : COL_TEXT_DIM;
                gfx_fill_rect(kx, ry, KW, KH, bg);

                if (c < nk) {
                    char lbl[2] = { krows[r][c], '\0' };
                    int tw, th;
                    TTF_SizeUTF8(g_font_list, lbl, &tw, &th);
                    gfx_draw_text(g_font_list, lbl,
                                  kx + (KW - tw) / 2, ry + (KH - th) / 2,
                                  fg, 0);
                } else {
                    int tw, th;
                    TTF_SizeUTF8(g_font_small, "DEL", &tw, &th);
                    gfx_draw_text(g_font_small, "DEL",
                                  kx + (KW - tw) / 2, ry + (KH - th) / 2,
                                  fg, 0);
                }
            }
        }

        /* Row 4: SPACE / CLR / OK */
        {
            int ry = KY0 + 4 * KRP;
            int rx = (SCREEN_WIDTH - 10 * (KW + KG) + KG) / 2;
            int tw_all = 10 * (KW + KG) - KG;
            int sw = 5 * (KW + KG) - KG;
            bool hl;

            hl = (!in_list && kb_r == 4 && kb_c == 0);
            gfx_fill_rect(rx, ry, sw, KH,
                          hl ? COL_HIGHLIGHT : COL_HEADER_BG);
            { int tw, th; TTF_SizeUTF8(g_font_list, "SPACE", &tw, &th);
              gfx_draw_text(g_font_list, "SPACE", rx + (sw - tw) / 2,
                            ry + (KH - th) / 2,
                            hl ? COL_TEXT : COL_TEXT_DIM, 0); }

            int cx = rx + sw + KG;
            int cw = 2 * KW + KG;
            hl = (!in_list && kb_r == 4 && kb_c == 1);
            gfx_fill_rect(cx, ry, cw, KH,
                          hl ? COL_HIGHLIGHT : COL_HEADER_BG);
            { int tw, th; TTF_SizeUTF8(g_font_list, "CLR", &tw, &th);
              gfx_draw_text(g_font_list, "CLR", cx + (cw - tw) / 2,
                            ry + (KH - th) / 2,
                            hl ? COL_TEXT : COL_TEXT_DIM, 0); }

            int dx = cx + cw + KG;
            int dw = rx + tw_all - dx;
            hl = (!in_list && kb_r == 4 && kb_c == 2);
            gfx_fill_rect(dx, ry, dw, KH,
                          hl ? COL_HIGHLIGHT : COL_SYSTEM_ICON);
            { int tw, th; TTF_SizeUTF8(g_font_list, "OK", &tw, &th);
              gfx_draw_text(g_font_list, "OK", dx + (dw - tw) / 2,
                            ry + (KH - th) / 2, COL_TEXT, 0); }
        }

        drm_flip();
        usleep(33333);
    }
}

/* =========================================================================
 * Game Launching
 * ========================================================================= */

static void launch_game(const char *rom_path, int system_idx, int state_slot)
{
    char core_path[MAX_NAME];
    snprintf(core_path, sizeof(core_path), "%s/%s",
             CORES_PATH, g_systems[system_idx].core_file);

    printf("LAUNCH: %s -L %s %s (slot=%d)\n", RETROARCH_BIN, core_path, rom_path, state_slot);

    /* Add to recently played */
    recents_add(rom_path);

    /* Save state before launching */
    state_save();

    /* Tear down all resources before fork */
    touch_cleanup();
    input_cleanup();
    gfx_cleanup();
    rga_cleanup();
    drm_cleanup();

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: ensure HOME is set so RetroArch finds its config */
        setenv("HOME", "/root", 1);
        /* exec retroarch with optional state slot */
        if (state_slot >= 0) {
            /*
             * RetroArch's savestate_auto_load looks for "<name>.state.auto",
             * not the regular "<name>.stateN" files.  Copy the state file
             * to the .auto path so auto-load finds it.
             * (Cannot use symlink — save states are on FAT32.)
             */
            char state_path[MAX_NAME];
            build_state_path(state_path, sizeof(state_path),
                             rom_path, g_systems[system_idx].core_file,
                             state_slot);

            /* Build the .auto path: <basename>.state.auto
             * Auto-load always uses the base savestate path + ".auto" */
            char auto_path[MAX_NAME];
            build_state_path(auto_path, sizeof(auto_path),
                             rom_path, g_systems[system_idx].core_file, 0);
            {
                size_t len = strlen(auto_path);
                snprintf(auto_path + len, sizeof(auto_path) - len, ".auto");
            }

            /* Copy state file to .auto path (FAT32 doesn't support symlinks) */
            unlink(auto_path);
            {
                FILE *src = fopen(state_path, "rb");
                FILE *dst = src ? fopen(auto_path, "wb") : NULL;
                if (src && dst) {
                    char buf[4096];
                    size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
                        fwrite(buf, 1, n, dst);
                }
                if (src) fclose(src);
                if (dst) fclose(dst);
            }

            /* Write temp config: enable auto-load and set slot for future saves */
            const char *tmp_cfg = "/tmp/ra_slot.cfg";
            FILE *fc = fopen(tmp_cfg, "w");
            if (fc) {
                fprintf(fc, "savestate_auto_load = \"true\"\n");
                fprintf(fc, "state_slot = \"%d\"\n", state_slot);
                fclose(fc);
            }
            execl(RETROARCH_BIN, "retroarch",
                  "-L", core_path,
                  "--appendconfig", tmp_cfg,
                  rom_path,
                  NULL);
        } else {
            execl(RETROARCH_BIN, "retroarch",
                  "-L", core_path,
                  rom_path,
                  NULL);
        }
        /* exec failed */
        fprintf(stderr, "LAUNCH: exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* Parent: wait for RetroArch to exit */
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        printf("LAUNCH: RetroArch exited with status %d\n",
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    } else {
        fprintf(stderr, "LAUNCH: fork failed: %s\n", strerror(errno));
    }

    /* Re-initialize everything */
    drm_init();
    rga_init();
    gfx_init();
    input_init();
    touch_init();

    /* Drain stale evdev events so the button used to exit RetroArch
       doesn't immediately re-trigger a game launch */
    {
        struct input_event ev;
        while (read(g_input.fd, &ev, sizeof(ev)) == sizeof(ev))
            ; /* discard */
        g_input.held = 0;
        g_input.pressed = 0;
        g_input.released = 0;
    }
}

/* =========================================================================
 * Key Repeat
 * ========================================================================= */

typedef struct {
    uint32_t mask;
    int      held_frames;
} RepeatState;

#define REPEAT_DELAY   12  /* Frames before repeat starts */
#define REPEAT_RATE    3   /* Frames between repeats */

static RepeatState g_repeat[4]; /* up, down, left, right */
static const uint32_t repeat_masks[] = {
    BTN_UP_MASK, BTN_DOWN_MASK, BTN_LEFT_MASK, BTN_RIGHT_MASK
};

static bool input_just_pressed_or_repeat(uint32_t mask)
{
    if (g_input.pressed & mask) return true;

    for (int i = 0; i < 4; i++) {
        if (repeat_masks[i] == mask) {
            if (g_input.held & mask) {
                g_repeat[i].held_frames++;
                if (g_repeat[i].held_frames > REPEAT_DELAY &&
                    (g_repeat[i].held_frames - REPEAT_DELAY) % REPEAT_RATE == 0)
                    return true;
            } else {
                g_repeat[i].held_frames = 0;
            }
            break;
        }
    }
    return false;
}

/* =========================================================================
 * Main
 * ========================================================================= */

static volatile bool g_running = true;

static void sig_handler(int sig)
{
    (void)sig;
    g_running = false;
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    setlinebuf(stdout);
    setlinebuf(stderr);

    struct timespec ts_start, ts_ready;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    printf("=== RetroOPI Launcher ===\n");
    printf("Platform: Orange Pi Zero3 (Allwinner H618), %dx%d\n",
           SCREEN_WIDTH, SCREEN_HEIGHT);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Initialize SDL (minimal - no video, just for SDL2_ttf/image) */
    SDL_Init(0);

    /* Apply default theme */
    theme_apply(0);

    if (drm_init() < 0) {
        fprintf(stderr, "FATAL: DRM init failed\n");
        return 1;
    }
    if (rga_init() < 0) {
        fprintf(stderr, "WARNING: RGA2 not available, using CPU rendering\n");
    }
    if (gfx_init() < 0) {
        fprintf(stderr, "FATAL: GFX init failed\n");
        rga_cleanup();
        drm_cleanup();
        return 1;
    }
    if (input_init() < 0) {
        fprintf(stderr, "WARNING: no gamepad, waiting for connection...\n");
        /* Don't fail - user might connect later */
    }
    touch_init();

    /* Load persistent data */
    ensure_data_dir();
    favorites_load();
    recents_load();

    /* Scan for available systems */
    menu_scan_systems();

    /* Restore saved state (theme, position) */
    state_load();

    /* Initial draw */
    ui_draw();

    clock_gettime(CLOCK_MONOTONIC, &ts_ready);
    double startup_ms = (ts_ready.tv_sec - ts_start.tv_sec) * 1000.0 +
                        (ts_ready.tv_nsec - ts_start.tv_nsec) / 1e6;
    printf("STARTUP: ready in %.0f ms\n", startup_ms);

    /* Main loop */
    int dirty = 0;
    while (g_running) {
        input_poll();
        dirty = 0;

        /* Navigation */
        if (input_just_pressed_or_repeat(BTN_UP_MASK)) {
            if (g_menu.selected > 0) {
                g_menu.selected--;
                dirty = 1;
            }
        }
        if (input_just_pressed_or_repeat(BTN_DOWN_MASK)) {
            if (g_menu.selected < g_menu.count - 1) {
                g_menu.selected++;
                dirty = 1;
            }
        }

        /* Page up/down with L1/R1 */
        if (g_input.pressed & BTN_L1_MASK) {
            g_menu.selected -= MAX_VISIBLE;
            if (g_menu.selected < 0) g_menu.selected = 0;
            dirty = 1;
        }
        if (g_input.pressed & BTN_R1_MASK) {
            g_menu.selected += MAX_VISIBLE;
            if (g_menu.selected >= g_menu.count)
                g_menu.selected = g_menu.count - 1;
            dirty = 1;
        }

        /* Left/Right: jump a letter at a time in the long lists.
         * Not in MENU_SYSTEMS (15 entries, pointless) and not in MENU_SETTINGS,
         * where Left/Right already adjust theme and volume. */
        if (g_menu.mode == MENU_ROMS || g_menu.mode == MENU_FAVORITES ||
            g_menu.mode == MENU_RECENTS) {
            if (input_just_pressed_or_repeat(BTN_LEFT_MASK))
                if (menu_jump_letter(-1)) dirty = 1;
            if (input_just_pressed_or_repeat(BTN_RIGHT_MASK))
                if (menu_jump_letter(+1)) dirty = 1;
        }

        /* Toggle favorite (X / Triangle) */
        if (g_input.pressed & BTN_X_MASK) {
            if ((g_menu.mode == MENU_ROMS || g_menu.mode == MENU_FAVORITES ||
                 g_menu.mode == MENU_RECENTS) && g_menu.count > 0) {
                MenuEntry *e = &g_menu.entries[g_menu.selected];
                if (e->path[0]) {
                    favorites_toggle(e->path);
                    e->is_favorite = favorites_contains(e->path);
                    /* If we're in favorites view and unfavorited, reload */
                    if (g_menu.mode == MENU_FAVORITES) {
                        int sel = g_menu.selected;
                        menu_load_favorites();
                        if (sel >= g_menu.count) sel = g_menu.count - 1;
                        if (sel < 0) sel = 0;
                        g_menu.selected = sel;
                    }
                    dirty = 1;
                }
            }
        }

        /* Confirm (A / Cross) */
        if (g_input.pressed & BTN_A_MASK) {
            if (g_menu.count > 0) {
                if (g_menu.mode == MENU_SYSTEMS) {
                    MenuEntry *e = &g_menu.entries[g_menu.selected];
                    /* so backing out lands here again, not at the top */
                    g_systems_return_id = e->system_idx;
                    if (e->system_idx == -100) {
                        /* Settings */
                        menu_load_settings();
                        dirty = 1;
                    } else if (e->system_idx == -101) {
                        /* Favorites */
                        menu_load_favorites();
                        dirty = 1;
                    } else if (e->system_idx == -102) {
                        /* Recents */
                        menu_load_recents();
                        dirty = 1;
                    } else {
                        int sel = g_menu.selected;
                        menu_scan_roms(sel);
                        dirty = 1;
                    }
                } else if (g_menu.mode == MENU_SETTINGS) {
                    int sel = g_menu.selected;
                    if (sel == SETTING_THEME) {
                        /* Cycle to next theme */
                        g_current_theme = (g_current_theme + 1) % NUM_THEMES;
                        theme_apply(g_current_theme);
                        state_save();
                        dirty = 1;
                    } else if (sel == SETTING_VOLUME) {
                        volume_apply(g_volume + 10);
                        state_save();
                        dirty = 1;
                    } else if (sel == SETTING_N64_QUALITY) {
                        n64_hires_set(!n64_hires_get());
                        dirty = 1;
                    } else if (sel == SETTING_FAVORITES) {
                        menu_load_favorites();
                        dirty = 1;
                    } else if (sel == SETTING_RECENTS) {
                        menu_load_recents();
                        dirty = 1;
                    } else if (sel == SETTING_CLEAR_RECENTS) {
                        g_recents_count = 0;
                        recents_save();
                        menu_load_settings();
                        dirty = 1;
                    }
                } else {
                    /* Launch game (ROMS, FAVORITES, or RECENTS mode) */
                    MenuEntry *e = &g_menu.entries[g_menu.selected];
                    if (e->system_idx >= 0) {
                        menu_remember_rom();
                        int slot = slot_picker(e->path, e->system_idx);
                        if (slot != -1) {
                            /* -2 = new game (no state), 0-9 = load slot */
                            launch_game(e->path, e->system_idx, slot == -2 ? -1 : slot);
                        }
                        /* After RetroArch exits or cancel, redraw */
                        dirty = 1;
                    }
                }
            }
        }

        /* Back (B / Circle) */
        if (g_input.pressed & BTN_B_MASK) {
            if (g_menu.mode == MENU_ROMS || g_menu.mode == MENU_FAVORITES ||
                g_menu.mode == MENU_RECENTS || g_menu.mode == MENU_SETTINGS) {
                menu_remember_rom();
                menu_scan_systems();
                menu_restore_systems_selection();
                dirty = 1;
            }
        }

        /* Search (Y / Square) */
        if (g_input.pressed & BTN_Y_MASK) {
            if (g_menu.mode != MENU_SETTINGS) {
                int result = search_run();
                if (result >= 0 && result < g_menu.count) {
                    g_menu.selected = result;
                    if (g_menu.selected < g_menu.scroll_top)
                        g_menu.scroll_top = g_menu.selected;
                    if (g_menu.selected >= g_menu.scroll_top + MAX_VISIBLE)
                        g_menu.scroll_top = g_menu.selected - MAX_VISIBLE + 1;
                }
                dirty = 1;
            }
        }

        /* Left/Right in Settings: cycle theme or adjust volume */
        if (g_menu.mode == MENU_SETTINGS && g_menu.selected == SETTING_THEME) {
            if (g_input.pressed & BTN_LEFT_MASK) {
                g_current_theme = (g_current_theme + NUM_THEMES - 1) % NUM_THEMES;
                theme_apply(g_current_theme);
                state_save();
                dirty = 1;
            }
            if (g_input.pressed & BTN_RIGHT_MASK) {
                g_current_theme = (g_current_theme + 1) % NUM_THEMES;
                theme_apply(g_current_theme);
                state_save();
                dirty = 1;
            }
        }
        if (g_menu.mode == MENU_SETTINGS && g_menu.selected == SETTING_N64_QUALITY) {
            if (g_input.pressed & (BTN_LEFT_MASK | BTN_RIGHT_MASK)) {
                n64_hires_set(!n64_hires_get());
                dirty = 1;
            }
        }
        if (g_menu.mode == MENU_SETTINGS && g_menu.selected == SETTING_VOLUME) {
            if (g_input.pressed & BTN_LEFT_MASK) {
                volume_apply(g_volume - 5);
                state_save();
                dirty = 1;
            }
            if (g_input.pressed & BTN_RIGHT_MASK) {
                volume_apply(g_volume + 5);
                state_save();
                dirty = 1;
            }
        }

        /* Expire the letter-jump overlay. The redraw at zero is what removes
         * it: without marking that frame dirty it would linger until something
         * else happened to trigger a draw. */
        if (g_jump_hint_frames > 0 && --g_jump_hint_frames == 0)
            dirty = 1;

        if (dirty) {
            ui_draw();
        }

        /* ~30 FPS polling rate */
        usleep(33333);
    }

    printf("Shutting down...\n");
    touch_cleanup();
    input_cleanup();
    gfx_cleanup();
    rga_cleanup();
    drm_cleanup();
    SDL_Quit();

    return 0;
}
