// ThumbyScummby — SDL implementation of engine/include/platform.h.
//
// Window, input, audio, file IO. The display target is the Thumby Color
// 128x128 RGB565 LCD; we render at native size and SDL upscales for the
// host preview.

#include "platform.h"
#include "mi_font_render.h"
#include "types.h"

#include <SDL2/SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>

namespace tsb::platform_sdl {

static const int kPreviewScale = 4;   // 4x scale of 128x128 = 512x512 window

// Game detected by load_data_dir().  Read by main.cpp at startup to
// pick the right DetectorResult.  Empty string until load_data_dir
// runs (or if no recognised layout was found).
//   "mi1"   — v4 floppy layout (000.LFL + DISK*.LEC)
//   "indy4" — v5 HD-installed, base name "atlantis"
//   "mi2"   — v5 HD-installed, base name "monkey2"
//   "v5"    — v5 HD-installed, any other base name (fallback)
char g_loaded_game[16] = "";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct State {
    SDL_Window   *win = nullptr;
    SDL_Renderer *ren = nullptr;
    SDL_Texture  *tex = nullptr;          // streaming RGB565 128x128
    uint16_t      framebuffer[DISPLAY_W * DISPLAY_H];

    // Loaded files (mmap'd, post-decrypted into a freshly allocated buffer)
    struct LoadedFile {
        uint8_t *data = nullptr;
        size_t   size = 0;
    };
    LoadedFile master;          // 000.LFL
    LoadedFile disk[4];         // DISK01-04.LEC (1-indexed; disk[0] unused)
    LoadedFile helper[5];       // 900-904.LFL (901-904 used, [0]/[1] unused)

    // Audio
    SDL_AudioDeviceID                audio_dev = 0;
    int                              audio_rate = 0;
    tsb::platform::AudioCallback     audio_cb = nullptr;
    void                            *audio_user = nullptr;

    // Input edge detection
    bool prev_a, prev_b, prev_lb, prev_rb, prev_menu;
    bool prev_ml, prev_mr;          // mouse left/right edge tracking

    // Quit flag
    bool quit = false;
};
static State g{};

// ---------------------------------------------------------------------------
// File loading helpers
// ---------------------------------------------------------------------------
//
// SCUMM v5 floppy game data is XOR'd with 0x69. We read the file fully into
// a heap buffer, XOR-decrypt, and keep that buffer alive for engine
// lifetime. The engine sees decrypted bytes via the Span returned from
// data_*().

static bool load_file(const char *path, uint8_t enc_byte,
                      uint8_t **out_data, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return false; }
    size_t sz = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { close(fd); return false; }
    ssize_t got = read(fd, buf, sz);
    close(fd);
    if (got != (ssize_t)sz) { free(buf); return false; }
    if (enc_byte) {
        for (size_t i = 0; i < sz; i++) buf[i] ^= enc_byte;
    }
    *out_data = buf;
    *out_size = sz;
    return true;
}

// MI1 VGA Floppy is SCUMM v4 resource format (small_header) but v5 engine.
// Encryption per the v4 rule:
//   room 0 (000.LFL)   -> NOT encrypted
//   rooms >= 900       -> NOT encrypted (charset / helpers)
//   other rooms (LECs) -> 0x69 XOR
bool load_data_dir(const char *path) {
    char buf[1024];

    auto try_open = [&](const char *fname_pattern, int fnum, uint8_t enc,
                        uint8_t **dst_data, size_t *dst_size) -> bool {
        snprintf(buf, sizeof(buf), fname_pattern, path, fnum);
        if (load_file(buf, enc, dst_data, dst_size)) return true;
        // Try lowercase variant
        char lower[1024]; strcpy(lower, buf);
        for (char *p = strrchr(lower, '/'); p && *p; p++) *p = (char)tolower(*p);
        return load_file(lower, enc, dst_data, dst_size);
    };

    // Try v4 floppy layout first: 000.LFL + DISK01-04.LEC + 9xx.LFL.
    snprintf(buf, sizeof(buf), "%s/000.LFL", path);
    bool have_v4 = load_file(buf, 0, &g.master.data, &g.master.size);
    if (!have_v4) {
        snprintf(buf, sizeof(buf), "%s/000.lfl", path);
        have_v4 = load_file(buf, 0, &g.master.data, &g.master.size);
    }

    if (have_v4) {
        snprintf(g_loaded_game, sizeof(g_loaded_game), "mi1");
        tsb::platform::log("loaded 000.LFL: %zu bytes (unencrypted)\n", g.master.size);

        // DISK01-04.LEC — kept RAW (encrypted). ScummFile::read applies
        // the 0x69 XOR per scummvm getEncByte() rules; pre-decrypting here
        // would double-XOR and scramble all bytes.
        for (int i = 1; i <= 4; i++) {
            if (try_open("%s/DISK%02d.LEC", i, 0, &g.disk[i-1].data, &g.disk[i-1].size)) {
                tsb::platform::log("loaded DISK%02d.LEC: %zu bytes (raw)\n", i, g.disk[i-1].size);
            } else {
                tsb::platform::log("warning: cannot open DISK%02d.LEC\n", i);
            }
        }

        // 901-904.LFL — unencrypted (charsets and helpers)
        for (int i = 901; i <= 904; i++) {
            if (try_open("%s/%d.LFL", i, 0, &g.helper[i-900].data, &g.helper[i-900].size)) {
                tsb::platform::log("loaded %d.LFL: %zu bytes (unencrypted)\n", i, g.helper[i-900].size);
            }
        }
        return true;
    }

    // v5 HD-installed: <BASE>.000 (index) + <BASE>.001 (data). Both
    // XOR-encrypted with 0x69; loaded RAW so ScummFile applies the XOR
    // per getEncByte() rules (v5 returns 0x69 for every room).
    {
        DIR *d = opendir(path);
        if (!d) {
            tsb::platform::log("error: cannot open dir %s\n", path);
            return false;
        }
        char base[256] = {0};
        struct dirent *de;
        while ((de = readdir(d))) {
            size_t n = strlen(de->d_name);
            if (n >= 4) {
                const char *suf = de->d_name + n - 4;
                if (strcasecmp(suf, ".000") == 0) {
                    snprintf(base, sizeof(base), "%.*s", (int)(n - 4), de->d_name);
                    break;
                }
            }
        }
        closedir(d);
        if (!base[0]) {
            tsb::platform::log("error: no v4 floppy or v5 HD layout found in %s\n", path);
            return false;
        }
        snprintf(buf, sizeof(buf), "%s/%s.000", path, base);
        if (!load_file(buf, 0, &g.master.data, &g.master.size)) {
            tsb::platform::log("error: cannot open %s.000\n", base);
            return false;
        }
        tsb::platform::log("loaded %s.000: %zu bytes (v5 HD, raw)\n", base, g.master.size);
        snprintf(buf, sizeof(buf), "%s/%s.001", path, base);
        if (!load_file(buf, 0, &g.disk[0].data, &g.disk[0].size)) {
            tsb::platform::log("error: cannot open %s.001\n", base);
            return false;
        }
        tsb::platform::log("loaded %s.001: %zu bytes (v5 HD, raw)\n", base, g.disk[0].size);
        // Pick the game tag from the base filename so main.cpp can
        // configure the right DetectorResult.
        if (strcasecmp(base, "atlantis") == 0)      snprintf(g_loaded_game, sizeof(g_loaded_game), "indy4");
        else if (strcasecmp(base, "monkey2") == 0)  snprintf(g_loaded_game, sizeof(g_loaded_game), "mi2");
        else                                        snprintf(g_loaded_game, sizeof(g_loaded_game), "v5");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------

bool init(int argc, char **argv) {
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    g.win = SDL_CreateWindow(
        "ThumbyScummby (host preview)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        DISPLAY_W * kPreviewScale, DISPLAY_H * kPreviewScale,
        SDL_WINDOW_SHOWN);
    if (!g.win) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return false; }

    g.ren = SDL_CreateRenderer(g.win, -1, SDL_RENDERER_ACCELERATED);
    if (!g.ren) g.ren = SDL_CreateRenderer(g.win, -1, SDL_RENDERER_SOFTWARE);
    if (!g.ren) { fprintf(stderr, "CreateRenderer: %s\n", SDL_GetError()); return false; }

    // SDL_RenderSetLogicalSize was unreliable under WSLg — output landed
    // at the top-left 128×128 of the window unscaled.  Use an explicit
    // dst-rect at present time + manual mouse-coord scaling instead.
    g.tex = SDL_CreateTexture(g.ren, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, DISPLAY_W, DISPLAY_H);
    if (!g.tex) { fprintf(stderr, "CreateTexture: %s\n", SDL_GetError()); return false; }
    return true;
}

void shutdown() {
    if (g.audio_dev) { SDL_CloseAudioDevice(g.audio_dev); g.audio_dev = 0; }
    if (g.tex) SDL_DestroyTexture(g.tex);
    if (g.ren) SDL_DestroyRenderer(g.ren);
    if (g.win) SDL_DestroyWindow(g.win);
    SDL_Quit();
    free(g.master.data);
    for (int i = 0; i < 4; i++) free(g.disk[i].data);
    for (int i = 0; i < 5; i++) free(g.helper[i].data);
    g = State{};
}

bool main_loop_iter() {
    if (g.quit) return false;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) g.quit = true;
    }
    // Frame pacing lives inside engine_tick() now — see scumm.cpp:2702-2857
    // (waitForTimer + go-loop port). The host's only job here is event
    // pumping + quit detection; the engine handles its own anchored wait
    // using VAR_TIMER_NEXT and the v4 PIT frequency.
    return !g.quit;
}

// ---------------------------------------------------------------------------
// SDL audio glue (thunks Engine callback)
// ---------------------------------------------------------------------------
static void sdl_audio_cb_thunk(void *user, Uint8 *stream, int len) {
    (void)user;
    if (g.audio_cb) {
        g.audio_cb(g.audio_user, (int16_t *)stream, len / 2);
    } else {
        memset(stream, 0, len);
    }
}

}  // namespace tsb::platform_sdl

// ---------------------------------------------------------------------------
// platform.h public API implementation
// ---------------------------------------------------------------------------
namespace tsb::platform {

Span data_master_index() {
    using namespace tsb::platform_sdl;
    return Span{g.master.data, g.master.size};
}
Span data_disk(int disk_id) {
    using namespace tsb::platform_sdl;
    if (disk_id < 1 || disk_id > 4) return Span{nullptr, 0};
    return Span{g.disk[disk_id-1].data, g.disk[disk_id-1].size};
}
Span data_helper(int id) {
    using namespace tsb::platform_sdl;
    if (id < 901 || id > 904) return Span{nullptr, 0};
    return Span{g.helper[id-900].data, g.helper[id-900].size};
}

// FIT / FILL / CROP scaling. Source: 320x200 8bpp paletted main + optional
// 320x200 paletted text overlay (sentinel 0xFD = transparent). Dest: 128x128
// RGB565 framebuffer.
//
// The main scene is averaged with the ThumbyNES md_core.c:601 packed-RGB565
// 2x2 box blend trick — three 32-bit adds + shifts + masks blend four
// pixels with no per-channel extract. The text overlay uses ink-priority
// sampling instead so 1-pixel glyph features (the shadow) survive the
// 320 -> 128 downsample (foreground > shadow > transparent).
static inline uint16_t pal_to_565(const uint8_t *pal, uint8_t idx) {
    uint8_t r = pal[idx*3 + 0];
    uint8_t g = pal[idx*3 + 1];
    uint8_t b = pal[idx*3 + 2];
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// 2x2 packed-RGB565 box blend (md_core.c:601). Returns avg of 4 src pixels.
static inline uint16_t blend4_565(uint16_t a, uint16_t b,
                                  uint16_t c, uint16_t d) {
    constexpr uint32_t MASK = 0x07E0F81Fu;
    uint32_t ea = ((uint32_t)a | ((uint32_t)a << 16)) & MASK;
    uint32_t eb = ((uint32_t)b | ((uint32_t)b << 16)) & MASK;
    uint32_t ec = ((uint32_t)c | ((uint32_t)c << 16)) & MASK;
    uint32_t ed = ((uint32_t)d | ((uint32_t)d << 16)) & MASK;
    uint32_t ab  = ((ea + eb) >> 1) & MASK;
    uint32_t cd  = ((ec + ed) >> 1) & MASK;
    uint32_t avg = ((ab + cd) >> 1) & MASK;
    return (uint16_t)((avg | (avg >> 16)) & 0xFFFFu);
}

// Resolve a single source pixel: text overlay if present (anything other
// than 0xFD = scummvm CHARSET_MASK_TRANSPARENCY, gfx.h:289), otherwise
// fall through to the main scene index.
static inline uint8_t resolve_src(uint8_t t, uint8_t v) {
    return (t != 0xFD) ? t : v;
}

// Scene-only layout uses kSceneSrcRows / kSceneLcdRows / kSentenceLcdY
// from platform.h.  Per-mode metrics are derived locally so they don't
// bloat the public header.
constexpr int kFitSceneLcdRows  = (kSceneSrcRows * DISPLAY_W) / VIRTUAL_SCREEN_W;  // 57
constexpr int kFitTopLB         = (kSceneLcdRows - kFitSceneLcdRows) / 2;          // 31
// Fill: vertical scale matches the legacy 200-row Fill (128 LCD / 200 src
// = 0.64×), giving 144 src × 0.64 = 92 LCD rows of scene pinned to the
// LCD top.  The remaining LCD rows up to the sentence strip are black,
// occupying the area where the verb panel used to be.
constexpr int kFillSceneLcdRows = (kSceneSrcRows * DISPLAY_H) / VIRTUAL_SCREEN_H;  // 92

// Source-coord → LCD-coord helpers used by the cursor blit.  No more
// panel split — source 0..143 maps continuously through per-mode math.
static inline int src_to_lcd_x(int src_x, ScaleMode mode, int crop_x) {
    if (mode == ScaleMode::Fill)
        return (src_x - crop_x) * DISPLAY_W / VIRTUAL_SCREEN_H;
    if (mode == ScaleMode::Crop)
        return src_x - crop_x;
    return src_x * DISPLAY_W / VIRTUAL_SCREEN_W;            // Fit
}
// Cursor's source-y → LCD-y must match the scene-blit math in
// present().  Scene blit uses src_y_max = panel_active ? 144 : 200, so
// the cursor formula has to use the same denominator — otherwise the
// cursor sprite drifts ~10-20 LCD pixels off the engine's hotspot
// position when panel_active flips.
static inline int src_to_lcd_y(int src_y, ScaleMode mode, int crop_y,
                                bool panel_active) {
    const int src_y_max = panel_active ? kSceneSrcRows : VIRTUAL_SCREEN_H;
    if (mode == ScaleMode::Crop) return src_y - crop_y;
    const int fit_rows  = (src_y_max * DISPLAY_W) / VIRTUAL_SCREEN_W;
    const int fill_rows = (src_y_max * DISPLAY_H) / VIRTUAL_SCREEN_H;
    int dst_h = (mode == ScaleMode::Fit) ? fit_rows : fill_rows;
    if (dst_h > kSceneLcdRows) dst_h = kSceneLcdRows;
    const int letterbox_top = (mode == ScaleMode::Fit)
                              ? (kSceneLcdRows - dst_h) / 2
                              : 0;
    return letterbox_top + src_y * dst_h / src_y_max;
}

// Render the cursor sprite onto the LCD framebuffer in pixel space.
// See device_pico/platform_pico.cpp for the design rationale — kept
// identical between hosts so behaviour matches between SDL and device.
static void blit_cursor_overlay(uint16_t *fb, const CursorInfo &c,
                                const uint8_t *palette,
                                ScaleMode mode, int crop_x, int crop_y,
                                bool panel_active) {
    if (!c.sprite || c.w <= 0 || c.h <= 0) return;

    int cw_lcd = c.w;
    int ch_lcd = c.h;
    (void)mode;
    if (cw_lcd < 4) cw_lcd = 4;
    if (ch_lcd < 4) ch_lcd = 4;

    const int anchor_lcd_x = src_to_lcd_x(c.x, mode, crop_x);
    const int anchor_lcd_y = src_to_lcd_y(c.y, mode, crop_y, panel_active);
    const int hsx_lcd = c.hotspot_x * cw_lcd / c.w;
    const int hsy_lcd = c.hotspot_y * ch_lcd / c.h;
    const int origin_x = anchor_lcd_x - hsx_lcd;
    const int origin_y = anchor_lcd_y - hsy_lcd;

    for (int ly = 0; ly < ch_lcd; ly++) {
        const int dy = origin_y + ly;
        // Clip cursor to scene area — the sentence strip below must
        // not be overpainted by a cursor straying out of the scene.
        if (dy < 0 || dy >= kSceneLcdRows) continue;
        int sy_lo = ly * c.h / ch_lcd;
        int sy_hi = ((ly + 1) * c.h + ch_lcd - 1) / ch_lcd;
        if (sy_hi <= sy_lo) sy_hi = sy_lo + 1;
        if (sy_hi > c.h)    sy_hi = c.h;

        uint16_t *drow = fb + dy * DISPLAY_W;
        for (int lx = 0; lx < cw_lcd; lx++) {
            const int dx = origin_x + lx;
            if (dx < 0 || dx >= DISPLAY_W) continue;
            int sx_lo = lx * c.w / cw_lcd;
            int sx_hi = ((lx + 1) * c.w + cw_lcd - 1) / cw_lcd;
            if (sx_hi <= sx_lo) sx_hi = sx_lo + 1;
            if (sx_hi > c.w)    sx_hi = c.w;

            uint8_t v = c.key_color;
            for (int sy = sy_lo; sy < sy_hi && v == c.key_color; sy++) {
                const uint8_t *srow = c.sprite + sy * c.w;
                for (int sx = sx_lo; sx < sx_hi; sx++) {
                    if (srow[sx] != c.key_color) { v = srow[sx]; break; }
                }
            }
            if (v == c.key_color) continue;
            drow[dx] = pal_to_565(palette, v);
        }
    }
}

// THUMBY-PORT — paint a glyph stamp at 75% source size (3:4) with an
// area-weighted blend against the existing framebuffer pixel.  Each
// LCD output pixel covers a 4/3 × 4/3 block of source-px (16 q-units
// in the denominator-3 fixed-point grid).  Per LCD pixel: 1–2 source
// pixels in each axis, weighted by overlap area (sum-of-weights = 16),
// summed in RGB565 component space and recomposed.
//
// First decode the bit-packed glyph into a small palette-index scratch,
// then walk LCD output pixels and integrate the source samples beneath
// each.  Foreground pixels resolve via cmap → palette → RGB565;
// background (color 0) defers to the current framebuffer pixel.
static inline void blit_text_stamp(uint16_t *fb,
                                   const TextStamp &s,
                                   const uint8_t *palette) {
    using namespace tsb::platform_sdl;       // for pal_to_565
    if (!s.charPtr) return;
    if (s.width == 0 || s.height == 0) return;
    if (s.width > 32 || s.height > 32) return;

    uint8_t glyph[32 * 32];
    {
        const uint8_t *src = s.charPtr;
        uint8_t bits = *src++;
        int     numbits = 8;
        const int bpp = s.bpp;
        for (int y = 0; y < s.height; y++) {
            for (int x = 0; x < s.width; x++) {
                glyph[y * 32 + x] = (uint8_t)((bits >> (8 - bpp)) & 0xFF);
                bits <<= bpp;
                numbits -= bpp;
                if (numbits == 0) {
                    bits = *src++;
                    numbits = 8;
                }
            }
        }
    }

    const int kNum = (s.scale_num > 0) ? (int)s.scale_num : 3;
    const int kDen = (s.scale_den > 0) ? (int)s.scale_den : 4;

    // 1:1 fast path — FullScale talk-area stamps, the 100% slider
    // setting, and every MI-overlay-font speech glyph (already at LCD
    // resolution).  All stamps share the palette+cmap path: cmap[1]
    // holds whichever talk-colour palette index the engine chose for
    // the line — identical for both SCUMM and MI glyphs.
    if (kNum == kDen || (s.flags & kTextStampFlagFullScale)) {
        for (int sy = 0; sy < s.height; sy++) {
            const int fb_y = s.dst_y + sy;
            if (fb_y < 0 || fb_y >= DISPLAY_H) continue;
            for (int sx = 0; sx < s.width; sx++) {
                const int fb_x = s.dst_x + sx;
                if (fb_x < 0 || fb_x >= DISPLAY_W) continue;
                const uint8_t c = glyph[sy * 32 + sx];
                if (c == 0) continue;        // transparent
                const int idx = (c < 4) ? s.cmap[c] : 0;
                fb[fb_y * DISPLAY_W + fb_x] = pal_to_565(palette, idx);
            }
        }
        return;
    }

    const int dst_w = (s.width  * kNum + kDen - 1) / kDen;
    const int dst_h = (s.height * kNum + kDen - 1) / kDen;
    // Each LCD pixel covers kDen×kDen weighted units in the q-grid.
    const int kWeightTotal = kDen * kDen;

    for (int dy = 0; dy < dst_h; dy++) {
        const int fb_y = s.dst_y + dy;
        if (fb_y < 0 || fb_y >= DISPLAY_H) continue;
        // Source y-range covered by this LCD row (q-units, denominator kNum).
        const int sy_lo_q = dy * kDen;
        const int sy_hi_q = sy_lo_q + kDen;
        const int sy_first =  sy_lo_q / kNum;
        const int sy_last  = (sy_hi_q - 1) / kNum;

        for (int dx = 0; dx < dst_w; dx++) {
            const int fb_x = s.dst_x + dx;
            if (fb_x < 0 || fb_x >= DISPLAY_W) continue;
            const int sx_lo_q = dx * kDen;
            const int sx_hi_q = sx_lo_q + kDen;
            const int sx_first =  sx_lo_q / kNum;
            const int sx_last  = (sx_hi_q - 1) / kNum;

            // Quick check: are ALL covered source pixels transparent?
            // If yes, skip the FB write entirely.
            bool any_ink = false;
            for (int sy = sy_first; sy <= sy_last && !any_ink; sy++) {
                if (sy >= s.height) break;
                for (int sx = sx_first; sx <= sx_last; sx++) {
                    if (sx >= s.width) break;
                    if (glyph[sy * 32 + sx]) { any_ink = true; break; }
                }
            }
            if (!any_ink) continue;

            const uint16_t bg = fb[fb_y * DISPLAY_W + fb_x];
            const int bg_r = (bg >> 11) & 0x1F;
            const int bg_g = (bg >> 5)  & 0x3F;
            const int bg_b =  bg        & 0x1F;
            int sum_r = 0, sum_g = 0, sum_b = 0;

            for (int sy = sy_first; sy <= sy_last; sy++) {
                if (sy >= s.height) break;
                const int wy = (sy_hi_q < (sy + 1) * kNum ? sy_hi_q : (sy + 1) * kNum)
                             - (sy_lo_q > (sy)     * kNum ? sy_lo_q : (sy)     * kNum);
                for (int sx = sx_first; sx <= sx_last; sx++) {
                    if (sx >= s.width) break;
                    const int wx = (sx_hi_q < (sx + 1) * kNum ? sx_hi_q : (sx + 1) * kNum)
                                 - (sx_lo_q > (sx)     * kNum ? sx_lo_q : (sx)     * kNum);
                    const int w = wx * wy;
                    const uint8_t c = glyph[sy * 32 + sx];
                    int r, g, b;
                    if (c) {
                        const int idx = (c < 4) ? s.cmap[c] : 0;
                        const uint16_t fg = pal_to_565(palette, idx);
                        r = (fg >> 11) & 0x1F;
                        g = (fg >> 5)  & 0x3F;
                        b =  fg        & 0x1F;
                    } else {
                        r = bg_r; g = bg_g; b = bg_b;
                    }
                    sum_r += r * w;
                    sum_g += g * w;
                    sum_b += b * w;
                }
            }
            const int out_r = sum_r / kWeightTotal;
            const int out_g = sum_g / kWeightTotal;
            const int out_b = sum_b / kWeightTotal;
            fb[fb_y * DISPLAY_W + fb_x] =
                (uint16_t)((out_r << 11) | (out_g << 5) | out_b);
        }
    }
}

void present(const uint8_t *virt, const uint8_t *text,
             const uint8_t *palette,
             ScaleMode mode, int crop_x, int crop_y,
             const CursorInfo *cursor,
             const TextStamp *text_stamps, int text_stamp_count,
             const char *sentence, int verb_prefix_len,
             bool send_to_lcd, bool panel_active,
             const char *cursor_tooltip) {
    // Source-row count visible this frame.  Panel-active gameplay
    // hides rows 144..199 (legacy panel area, now in overlay UI);
    // cutscene / map / title screens show the full 0..199 source.
    const int src_y_max = panel_active ? kSceneSrcRows : VIRTUAL_SCREEN_H;
    using namespace tsb::platform_sdl;
    uint16_t *fb = g.framebuffer;

    memset(fb, 0, sizeof(g.framebuffer));

    auto blit_row_blend = [&](int lcd_row, int sy, int sy2,
                              const uint16_t *xa, const uint16_t *xb) {
        const uint8_t *vrow1 = virt + sy  * VIRTUAL_SCREEN_W;
        const uint8_t *vrow2 = virt + sy2 * VIRTUAL_SCREEN_W;
        const uint8_t *trow1 = text ? text + sy  * VIRTUAL_SCREEN_W : nullptr;
        const uint8_t *trow2 = text ? text + sy2 * VIRTUAL_SCREEN_W : nullptr;
        uint16_t *drow = fb + lcd_row * DISPLAY_W;
        for (int dx = 0; dx < DISPLAY_W; dx++) {
            int sx = xa[dx], sx2 = xb[dx];
            uint8_t s_a = trow1 ? resolve_src(trow1[sx],  vrow1[sx])  : vrow1[sx];
            uint8_t s_b = trow1 ? resolve_src(trow1[sx2], vrow1[sx2]) : vrow1[sx2];
            uint8_t s_c = trow2 ? resolve_src(trow2[sx],  vrow2[sx])  : vrow2[sx];
            uint8_t s_d = trow2 ? resolve_src(trow2[sx2], vrow2[sx2]) : vrow2[sx2];
            drow[dx] = blend4_565(
                pal_to_565(palette, s_a),
                pal_to_565(palette, s_b),
                pal_to_565(palette, s_c),
                pal_to_565(palette, s_d));
        }
    };

    // ---------- Scene region (LCD rows 0..119) ----------
    //   Panel-active (gameplay): show source 0..143
    //   Panel-inactive (cutscene/map/title): show source 0..199
    //
    //   Fit:   width-fit isotropic 0.4× — 57 (panel) / 80 (full) LCD rows
    //          centred-letterboxed in the 120-row scene area
    //   Fill:  0.64× vertical (legacy ratio) — 92 (panel) / 128-clipped
    //          (full) LCD rows pinned to LCD top
    //   Crop:  1:1 native, pannable
    if (mode == ScaleMode::Fit || mode == ScaleMode::Fill) {
        const int fit_rows  = (src_y_max * DISPLAY_W) / VIRTUAL_SCREEN_W;  // 57 or 80
        const int fill_rows = (src_y_max * DISPLAY_H) / VIRTUAL_SCREEN_H;  // 92 or 128
        int dst_h           = (mode == ScaleMode::Fit) ? fit_rows : fill_rows;
        if (dst_h > kSceneLcdRows) dst_h = kSceneLcdRows;   // never overrun strip
        const int letterbox_top = (mode == ScaleMode::Fit)
                                  ? (kSceneLcdRows - dst_h) / 2
                                  : 0;
        uint16_t sxa[DISPLAY_W], sxb[DISPLAY_W];
        if (mode == ScaleMode::Fill) {
            int pan_max = VIRTUAL_SCREEN_W - (DISPLAY_W * VIRTUAL_SCREEN_H / DISPLAY_H);
            if (crop_x < 0)        crop_x = 0;
            if (crop_x > pan_max)  crop_x = pan_max;
            for (int dx = 0; dx < DISPLAY_W; dx++) {
                int sx  = crop_x + (dx * VIRTUAL_SCREEN_H) / DISPLAY_H;
                int sx2 = sx + 1; if (sx2 >= VIRTUAL_SCREEN_W) sx2 = sx;
                sxa[dx] = (uint16_t)sx;
                sxb[dx] = (uint16_t)sx2;
            }
        } else {  // Fit — width-fit, no x pan
            for (int dx = 0; dx < DISPLAY_W; dx++) {
                int sx  = (dx * VIRTUAL_SCREEN_W) / DISPLAY_W;
                int sx2 = sx + 1; if (sx2 >= VIRTUAL_SCREEN_W) sx2 = sx;
                sxa[dx] = (uint16_t)sx;
                sxb[dx] = (uint16_t)sx2;
            }
        }
        for (int dy = 0; dy < dst_h; dy++) {
            int sy  = (dy * src_y_max) / dst_h;
            int sy2 = sy + 1; if (sy2 >= src_y_max) sy2 = sy;
            blit_row_blend(letterbox_top + dy, sy, sy2, sxa, sxb);
        }
    } else { // Crop — 1:1 native, scene window 128×120
        if (crop_x < 0) crop_x = 0;
        if (crop_y < 0) crop_y = 0;
        const int crop_x_max = VIRTUAL_SCREEN_W - DISPLAY_W;
        const int crop_y_max = src_y_max - kSceneLcdRows;     // 24 (panel) or 80 (full)
        if (crop_x > crop_x_max) crop_x = crop_x_max;
        if (crop_y > crop_y_max) crop_y = crop_y_max;
        for (int dy = 0; dy < kSceneLcdRows; dy++) {
            const uint8_t *srow = virt + (crop_y + dy) * VIRTUAL_SCREEN_W + crop_x;
            const uint8_t *trow = text
                ? text + (crop_y + dy) * VIRTUAL_SCREEN_W + crop_x
                : nullptr;
            uint16_t *drow = fb + dy * DISPLAY_W;
            for (int dx = 0; dx < DISPLAY_W; dx++) {
                uint8_t t = trow ? trow[dx] : 0xFD;
                drow[dx] = pal_to_565(palette, (t != 0xFD) ? t : srow[dx]);
            }
        }
    }

    // ---------- LCD-native glyph stamps (talk-area only) ----------
    // Stamps with dst_y >= kSentenceLcdY are silently dropped — the
    // sentence strip is owned by the MI-font path below, not by stamps.
    for (int i = 0; i < text_stamp_count; i++) {
        const TextStamp &s = text_stamps[i];
        if (s.dst_y >= kSentenceLcdY) continue;
        blit_text_stamp(fb, s, palette);
    }

    // ---------- Cursor (clipped to scene area) ----------
    if (cursor) blit_cursor_overlay(fb, *cursor, palette,
                                     mode, crop_x, crop_y, panel_active);

    // ---------- Cursor tooltip (auto-verb hover label) ----------
    if (cursor && cursor_tooltip && cursor_tooltip[0]) {
        const int anchor_x = src_to_lcd_x(cursor->x, mode, crop_x);
        const int anchor_y = src_to_lcd_y(cursor->y, mode, crop_y, panel_active);
        const int tw = tsb::mi_font::text_width(cursor_tooltip);
        constexpr uint16_t kTipColor = 0xCE2C;     // MI1 highlight #cec760
        // Default position: just below-right of the cursor.  Flip to
        // left of cursor if it'd run off the right edge; flip above
        // if it'd hit the sentence strip.
        int tx = anchor_x + 6;
        int ty = anchor_y + 4;
        if (tx + tw > DISPLAY_W - 1) tx = anchor_x - tw - 4;
        if (ty + 8 > kSentenceLcdY)  ty = anchor_y - 9;
        if (tx < 1) tx = 1;
        if (ty < 1) ty = 1;
        tsb::mi_font::draw(tx, ty, cursor_tooltip, kTipColor);
    }

    // ---------- Sentence strip (LCD rows 120..127) ----------
    // Always painted last so it sits on top of anything that strayed
    // near the boundary.  Verb prefix in accent yellow, noun body in
    // white.  Empty sentence → strip stays black.
    for (int y = kSentenceLcdY; y < DISPLAY_H; y++) {
        for (int x = 0; x < DISPLAY_W; x++) fb[y * DISPLAY_W + x] = 0;
    }
    if (sentence && sentence[0]) {
        // MI1 sentence-line palette: blue throughout (#0099aa → 0x04D5).
        // Verb prefix and noun both render in the same blue; we keep
        // the prefix split for any future highlight effects.
        constexpr uint16_t kAccent = 0x04D5;     // MI1 sentence #0099aa
        constexpr uint16_t kBody   = 0x04D5;
        constexpr int kMargin = 2;
        int total_len = 0;
        for (const char *p = sentence; *p; p++) total_len++;
        const int text_w = tsb::mi_font::text_width(sentence);
        static uint32_t s_frame = 0; ++s_frame;
        const int scroll = tsb::mi_font::marquee_offset(
            text_w, DISPLAY_W - kMargin * 2, s_frame);
        const int origin_x = kMargin - scroll;
        tsb::mi_font::draw_substr(origin_x, kSentenceLcdY, sentence,
                                  0, verb_prefix_len, kAccent);
        tsb::mi_font::draw_substr(origin_x, kSentenceLcdY, sentence,
                                  verb_prefix_len, total_len, kBody);
    }

    // DEBUG: periodic PPM dump for offline inspection.  Set env var
    // THUMBY_DUMP_DIR to a directory; every Nth frame (default 30,
    // override with THUMBY_DUMP_EVERY) is written as PPM.
    {
        static int s_dump_frame_count = 0;
        static const char *s_dump_dir = nullptr;
        static int s_dump_every = 0;
        static bool s_init_done = false;
        if (!s_init_done) {
            s_init_done = true;
            s_dump_dir = getenv("THUMBY_DUMP_DIR");
            const char *every = getenv("THUMBY_DUMP_EVERY");
            s_dump_every = every ? atoi(every) : 30;
            if (s_dump_every <= 0) s_dump_every = 30;
        }
        if (s_dump_dir) {
            if ((s_dump_frame_count % s_dump_every) == 0) {
                char path[512];
                snprintf(path, sizeof(path), "%s/frame_%06d.ppm",
                         s_dump_dir, s_dump_frame_count);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", DISPLAY_W, DISPLAY_H);
                    for (int p = 0; p < DISPLAY_W * DISPLAY_H; p++) {
                        const uint16_t px = fb[p];
                        const uint8_t r5 = (px >> 11) & 0x1F;
                        const uint8_t g6 = (px >> 5)  & 0x3F;
                        const uint8_t b5 =  px        & 0x1F;
                        const uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
                        const uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
                        const uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));
                        fputc(r8, f); fputc(g8, f); fputc(b8, f);
                    }
                    fclose(f);
                }
            }
            s_dump_frame_count++;
        }
    }

    if (send_to_lcd) {
        SDL_UpdateTexture(g.tex, nullptr, fb, DISPLAY_W * 2);
        SDL_RenderClear(g.ren);
        int rw = 0, rh = 0;
        SDL_GetRendererOutputSize(g.ren, &rw, &rh);
        SDL_Rect dst{0, 0, rw, rh};
        SDL_RenderCopy(g.ren, g.tex, nullptr, &dst);
        SDL_RenderPresent(g.ren);
    }
}

bool poll_input(Input *out) {
    using namespace tsb::platform_sdl;
    if (g.quit) return false;
    SDL_PumpEvents();
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);

    // DEBUG: scripted input driver.  Set THUMBY_INPUT_SCRIPT to a
    // semicolon-separated list of "<delay_ms>:<key>" entries (key one
    // of: esc, b, a, menu, m).  When the elapsed time since boot
    // exceeds the entry's delay, that key is held for one poll cycle.
    // Useful for unattended SDL runs that need to skip past intro
    // screens to reach a target scene before frame-dumping.
    static Uint32 s_boot_ms = 0;
    static const char *s_script = nullptr;
    static bool s_script_init = false;
    if (!s_script_init) {
        s_script_init = true;
        s_script = getenv("THUMBY_INPUT_SCRIPT");
        s_boot_ms = SDL_GetTicks();
    }
    bool scripted_esc = false, scripted_b = false, scripted_a = false;
    bool scripted_menu = false;
    if (s_script) {
        const Uint32 now = SDL_GetTicks() - s_boot_ms;
        const char *p = s_script;
        while (*p) {
            int delay = atoi(p);
            const char *colon = strchr(p, ':');
            if (!colon) break;
            const char *next = strchr(colon + 1, ';');
            const size_t key_len = next ? (size_t)(next - colon - 1) : strlen(colon + 1);
            // Fire while in a small post-delay window (250 ms) so the
            // engine catches it as a single press+release.
            if (now >= (Uint32)delay && now < (Uint32)(delay + 250)) {
                if (key_len == 3 && !strncmp(colon + 1, "esc", 3))   scripted_esc = true;
                if (key_len == 1 && colon[1] == 'b')                  scripted_b = true;
                if (key_len == 1 && colon[1] == 'a')                  scripted_a = true;
                if (key_len == 4 && !strncmp(colon + 1, "menu", 4))   scripted_menu = true;
            }
            if (!next) break;
            p = next + 1;
        }
    }

    out->dpad_up    = keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP];
    out->dpad_down  = keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN];
    out->dpad_left  = keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT];
    out->dpad_right = keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT];

    bool a    = keys[SDL_SCANCODE_PERIOD] || keys[SDL_SCANCODE_J] || scripted_a;
    bool b    = keys[SDL_SCANCODE_COMMA]  || keys[SDL_SCANCODE_K] || scripted_b;
    bool lb   = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_Q];
    bool rb   = keys[SDL_SCANCODE_SPACE]  || keys[SDL_SCANCODE_E];
    // RETURN / M map to MENU (cycle scale mode). ESC is its own input,
    // delivered through Input.escape_pressed (cutscene-exit on host).
    bool menu = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_M] || scripted_menu;
    bool esc  = keys[SDL_SCANCODE_ESCAPE] || scripted_esc;

    out->button_a = a;       out->button_b = b;
    out->button_lb = lb;     out->button_rb = rb;
    out->button_menu = menu;

    out->a_pressed   = a    && !g.prev_a;
    out->b_pressed   = b    && !g.prev_b;
    out->lb_pressed  = lb   && !g.prev_lb;
    out->rb_pressed  = rb   && !g.prev_rb;
    out->menu_pressed= menu && !g.prev_menu;
    static bool prev_esc = false;
    out->escape_pressed = esc && !prev_esc;
    prev_esc = esc;

    out->a_released   = !a    && g.prev_a;
    out->b_released   = !b    && g.prev_b;
    out->lb_released  = !lb   && g.prev_lb;
    out->rb_released  = !rb   && g.prev_rb;
    out->menu_released= !menu && g.prev_menu;

    g.prev_a = a; g.prev_b = b;
    g.prev_lb = lb; g.prev_rb = rb;
    g.prev_menu = menu;

    // Mouse — reports in DISPLAY-space (0..127). The engine maps to
    // SCUMM 320x200 game coords using its known scale_mode (since the
    // platform doesn't know FIT vs FILL vs CROP). SDL_RenderWindowToLogical
    // accounts for SDL_RenderSetLogicalSize, so logical coords match
    // what the renderer treats as the 128x128 framebuffer.
    int wx, wy;
    Uint32 btnmask = SDL_GetMouseState(&wx, &wy);
    float lx, ly;
    SDL_RenderWindowToLogical(g.ren, wx, wy, &lx, &ly);
    int mx = (int)lx, my = (int)ly;
    if (mx < 0) mx = 0;
    if (mx > DISPLAY_W - 1) mx = DISPLAY_W - 1;
    if (my < 0) my = 0;
    if (my > DISPLAY_H - 1) my = DISPLAY_H - 1;
    bool ml = (btnmask & SDL_BUTTON_LMASK) != 0;
    bool mr = (btnmask & SDL_BUTTON_RMASK) != 0;
    out->mouse_present = true;
    out->mouse_x = mx;
    out->mouse_y = my;
    out->mouse_left  = ml;
    out->mouse_right = mr;
    out->mouse_left_pressed   =  ml && !g.prev_ml;
    out->mouse_right_pressed  =  mr && !g.prev_mr;
    out->mouse_left_released  = !ml &&  g.prev_ml;
    out->mouse_right_released = !mr &&  g.prev_mr;
    g.prev_ml = ml;
    g.prev_mr = mr;
    return true;
}

int audio_init(int requested_rate, AudioCallback cb, void *user) {
    using namespace tsb::platform_sdl;
    SDL_AudioSpec want{}, have{};
    want.freq = requested_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    // 512 samples @ 22050 Hz = ~23ms per audio callback. iMUSE events
    // get dispatched at audio-buffer boundaries (we tick imuse for the
    // chunk's worth of time at the START of each callback), so the
    // buffer size IS the timing-quantization granularity for note
    // events. The earlier 2048-sample (~93ms) buffer was audible as
    // jittery pacing — multiple events that should be ~30ms apart got
    // collapsed into the same callback boundary. 512 still has 23ms
    // of dropout slack — far more than dbopl + iMUSE actually need
    // (we measured ~300us per 2048-sample callback earlier, so 512
    // costs ~75us of the 23000us budget, comfortable).
    want.samples = 512;
    want.callback = sdl_audio_cb_thunk;
    g.audio_cb = cb;
    g.audio_user = user;
    g.audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g.audio_dev) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return 0;
    }
    g.audio_rate = have.freq;
    fprintf(stderr, "audio: requested %d Hz / %d samples; got %d Hz / %d samples / %d ch / fmt=0x%04X\n",
            want.freq, want.samples,
            have.freq, have.samples, have.channels, (unsigned)have.format);
    SDL_PauseAudioDevice(g.audio_dev, 0);
    return have.freq;
}

void audio_shutdown() {
    using namespace tsb::platform_sdl;
    if (g.audio_dev) { SDL_CloseAudioDevice(g.audio_dev); g.audio_dev = 0; }
}

uint32_t millis() { return SDL_GetTicks(); }
uint32_t micros() { return (uint32_t)(SDL_GetPerformanceCounter() *
                                      1000000ull / SDL_GetPerformanceFrequency()); }
void sleep_ms(uint32_t ms) { SDL_Delay(ms); }

void log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void log_flush() { fflush(stderr); }

// Host has no in-game LOG viewer (log goes to stderr already).  Stub
// the accessors so the device-side viewer code compiles unchanged.
int  log_history_count() { return 0; }
void log_history_get(int idx, char *out, int outsz) {
    (void)idx;
    if (out && outsz > 0) out[0] = '\0';
}

[[noreturn]] void panic(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "PANIC: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

// Boot diagnostic splash — device prints a coloured frame to localise
// a hang inside engine_init. Host log mirror; full splash on host
// would need a window+pump cycle that's not worth it for the device-
// only debug aid.
void debug_splash(uint16_t rgb565) {
    fprintf(stderr, "[debug_splash] color=0x%04X\n", rgb565);
}

// Heap-tracked boot checkpoint.  On host we ignore the colour and just
// log `label` plus the current heap-in-use figure from mallinfo2() so we
// can see exactly which engine init step blows the RP2350's heap.
void checkpoint(const char *label, uint16_t /*color*/) {
#if defined(__GLIBC__)
    struct mallinfo2 mi = mallinfo2();
    // Total = uordblks (arena, in-use) + hblkhd (mmap'd large allocs).
    // Big >128 KB allocations go via mmap and DON'T show up in uordblks.
    size_t total = (size_t)mi.uordblks + (size_t)mi.hblkhd;
    fprintf(stderr,
            "[ckpt] %-32s total=%6zu KB  uord=%6zu KB  mmap=%6zu KB  arena=%6zu KB\n",
            label,
            total              / 1024,
            (size_t)mi.uordblks / 1024,
            (size_t)mi.hblkhd  / 1024,
            (size_t)mi.arena   / 1024);
#else
    fprintf(stderr, "[ckpt] %s\n", label);
#endif
}

// SDL audio is driven by SDL's own thread via the registered callback;
// no per-frame refill needed.
void audio_pump() {}

// ---------------------------------------------------------------------------
// Save/Load menu primitives — direct LCD framebuffer paint.
// ---------------------------------------------------------------------------

void lcd_fill(uint16_t rgb565) {
    using namespace tsb::platform_sdl;
    for (int i = 0; i < DISPLAY_W * DISPLAY_H; i++) {
        g.framebuffer[i] = rgb565;
    }
}

void lcd_pixel(int x, int y, uint16_t rgb565) {
    using namespace tsb::platform_sdl;
    if ((unsigned)x >= (unsigned)DISPLAY_W) return;
    if ((unsigned)y >= (unsigned)DISPLAY_H) return;
    g.framebuffer[y * DISPLAY_W + x] = rgb565;
}

void lcd_present_now() {
    using namespace tsb::platform_sdl;
    SDL_UpdateTexture(g.tex, nullptr, g.framebuffer, DISPLAY_W * 2);
    SDL_RenderClear(g.ren);
    int rw = 0, rh = 0;
    SDL_GetRendererOutputSize(g.ren, &rw, &rh);
    SDL_Rect dst{0, 0, rw, rh};
    SDL_RenderCopy(g.ren, g.tex, nullptr, &dst);
    SDL_RenderPresent(g.ren);
}

bool is_lb_held() {
    SDL_PumpEvents();
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    return keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_Q];
}

bool is_rb_held() {
    SDL_PumpEvents();
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    return keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_E];
}

bool is_menu_held() {
    SDL_PumpEvents();
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    return keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_M];
}

void lcd_dim_box(int x, int y, int w, int h) {
    using namespace tsb::platform_sdl;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_W) w = DISPLAY_W - x;
    if (y + h > DISPLAY_H) h = DISPLAY_H - y;
    if (w <= 0 || h <= 0) return;
    for (int dy = 0; dy < h; dy++) {
        uint16_t *row = g.framebuffer + (y + dy) * DISPLAY_W + x;
        for (int dx = 0; dx < w; dx++) {
            // Halve each RGB565 channel: shift right and clear borrow bits.
            row[dx] = (row[dx] >> 1) & 0x7BEFu;
        }
    }
}

}  // namespace tsb::platform
