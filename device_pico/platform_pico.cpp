// ThumbyScummby — Pico SDK implementation of engine/include/platform.h.
//
// Bare-metal Thumby Color (RP2350) target. Game data .incbin'd in flash,
// 128x128 RGB565 LCD via GC9107 + DMA, GPIO buttons, dual-core PWM audio
// (core1 mixer feeds the PWM ring fed by an IRQ-driven 22050 Hz timer).

#include "platform.h"
#include "mi_font_render.h"
#include "types.h"

#include "lcd_gc9107.h"
#include "buttons.h"
#include "audio_pwm.h"

extern "C" {
#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
}

#include <stdarg.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>

// ---------------------------------------------------------------------------
// .incbin'd data blob (see data_section.S, tools/pack_device.py).
// ---------------------------------------------------------------------------
extern "C" {
    extern const uint8_t tsb_data_blob[];
    extern const uint8_t tsb_data_blob_end[];
}

namespace tsb::platform_pico {

// Blob layout (see tools/pack_device.py):
//   off 0:  'TSDB' magic
//   off 4:  version (uint32 LE)
//   off 8:  9 entries x (uint32 offset, uint32 size)
//   off 80+: file bodies
constexpr int kNumEntries = 9;
constexpr int kHeaderSize = 8 + kNumEntries * 8;

struct Entry {
    uint32_t offset;
    uint32_t size;
};

static Entry g_entries[kNumEntries];
static bool  g_blob_ok = false;

static void parse_blob() {
    const uint8_t *p = tsb_data_blob;
    if (p[0] != 'T' || p[1] != 'S' || p[2] != 'D' || p[3] != 'B') {
        g_blob_ok = false;
        return;
    }
    // version at offset 4 (we don't enforce yet)
    for (int i = 0; i < kNumEntries; i++) {
        const uint8_t *e = p + 8 + i * 8;
        uint32_t off  = (uint32_t)e[0] | ((uint32_t)e[1] << 8) |
                        ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
        uint32_t size = (uint32_t)e[4] | ((uint32_t)e[5] << 8) |
                        ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
        g_entries[i].offset = off;
        g_entries[i].size   = size;
    }
    g_blob_ok = true;
}

// ---------------------------------------------------------------------------
// Framebuffer + present helpers
// ---------------------------------------------------------------------------
//
// Engine renders to a 320x200 8bpp virtual screen + 256x3 palette and calls
// platform::present() once per frame. We do FIT/FILL/CROP scaling here and
// blit into a 128x128 RGB565 framebuffer that the LCD DMA pushes out.

static uint16_t g_fb[DISPLAY_W * DISPLAY_H];
// Two-frame scratch isn't strictly needed since the DMA reads directly from
// g_fb and lcd_present() waits for any prior DMA before reuse — but having
// the buffer sized to whole pixels is enough.

static inline uint16_t pal_to_565(const uint8_t *pal, uint8_t idx) {
    uint8_t r = pal[idx*3 + 0];
    uint8_t g = pal[idx*3 + 1];
    uint8_t b = pal[idx*3 + 2];
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ---------------------------------------------------------------------------
// Audio: single-core. The PWM IRQ on core 0 drains the ring per-sample
// at 22050 Hz (audio_irq in audio_pwm.c). The engine's per-frame
// audio_pump() refills the ring with samples synthesised by the mixer
// callback. Core 1 is left unused — multicore had been hanging
// engine_init via multicore_launch_core1's FIFO handshake.
// ---------------------------------------------------------------------------

static tsb::platform::AudioCallback g_audio_cb = nullptr;
static void                        *g_audio_user = nullptr;
static int                          g_audio_rate = 22050;

// Mix buffer reused across audio_pump() calls. Sized for a frame's
// worth of audio at 22050 Hz: ~735 samples per 30 fps frame, but we
// top up greedily so 1024 gives 1.5 frames of slack within one pump
// invocation. Static-storage to avoid the on-stack allocation.
constexpr int kMixChunkSamples = 1024;
static int16_t s_mix_buf[kMixChunkSamples];

// ---------------------------------------------------------------------------
// Input edge detection + debouncing.  poll_input runs once per
// updateScreen, which fires several times per scummLoop tick during the
// engine's waitForTimer wait — well over 100 Hz.  Tactile button bounce
// (5-10 ms typical) fires multiple raw edges per physical press without
// a stability filter, which the SCUMM engine sees as multiple clicks
// (e.g. clicking a dialog choice ALSO dismisses the resulting actor
// speech).  We track raw state changes against a 20 ms stability
// window: edges fire only on the filtered state.
// ---------------------------------------------------------------------------
constexpr uint32_t kDebounceMs = 20;

struct DebouncedButton {
    bool     last_raw    = false;
    bool     stable      = false;
    bool     prev_stable = false;
    uint32_t last_change = 0;
};

static DebouncedButton g_db_a, g_db_b, g_db_lb, g_db_rb, g_db_menu;
static DebouncedButton g_db_up, g_db_down, g_db_left, g_db_right;

static inline void debounce_step(DebouncedButton &b, bool raw, uint32_t now_ms) {
    if (raw != b.last_raw) {
        b.last_raw    = raw;
        b.last_change = now_ms;
    }
    if (now_ms - b.last_change >= kDebounceMs) {
        b.stable = raw;
    }
}

}  // namespace tsb::platform_pico

// ---------------------------------------------------------------------------
// Public API: implements platform.h
// ---------------------------------------------------------------------------
namespace tsb::platform {

Span data_master_index() {
    using namespace tsb::platform_pico;
    if (!g_blob_ok) return Span{nullptr, 0};
    return Span{tsb_data_blob + g_entries[0].offset, g_entries[0].size};
}

Span data_disk(int disk_id) {
    using namespace tsb::platform_pico;
    if (!g_blob_ok || disk_id < 1 || disk_id > 4) return Span{nullptr, 0};
    int idx = disk_id;  // entries 1..4 are disks 1..4
    return Span{tsb_data_blob + g_entries[idx].offset, g_entries[idx].size};
}

Span data_helper(int id) {
    using namespace tsb::platform_pico;
    if (!g_blob_ok || id < 901 || id > 904) return Span{nullptr, 0};
    int idx = 5 + (id - 901);  // entries 5..8 are helpers 901..904
    return Span{tsb_data_blob + g_entries[idx].offset, g_entries[idx].size};
}

// 2x2 packed-RGB565 box blend (md_core.c:601 — same trick as ThumbyNES).
// Returns avg of 4 src pixels with one 32-bit add+shift+AND per channel.
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

// Unified LCD layout — verb panel pinned to the LCD bottom in ALL
// modes so its visual position stays put as the user cycles Fit / Fill
// / Crop.  Only the SCENE region above changes between modes:
//
//   Fit  — top letterbox 24 + scene 58 (Fit Y 0.4×) + gap 10 + verb 36
//   Fill — scene 92 (isotropic 0.64×, no letterbox) + verb 36
//   Crop — scene 92 (1:1 native, panned via crop_x/y) + verb 36
//
// Scene-only layout — see host_sdl/platform_sdl.cpp for the rationale.
// Source 320×144 → LCD 0..119; sentence strip occupies LCD 120..127.
constexpr int kFitSceneLcdRows  = (kSceneSrcRows * DISPLAY_W) / VIRTUAL_SCREEN_W;  // 57
constexpr int kFitTopLB         = (kSceneLcdRows - kFitSceneLcdRows) / 2;          // 31
// Fill: legacy "medium zoom" — 0.64× vertical scale matching the OLD
// 200-row Fill, pinned to LCD top so the black gap (former panel area)
// sits between scene and sentence strip.
constexpr int kFillSceneLcdRows = (kSceneSrcRows * DISPLAY_H) / VIRTUAL_SCREEN_H;  // 92

static inline int src_to_lcd_x(int src_x, ScaleMode mode, int crop_x) {
    if (mode == ScaleMode::Fill)
        return (src_x - crop_x) * DISPLAY_W / VIRTUAL_SCREEN_H;
    if (mode == ScaleMode::Crop)
        return src_x - crop_x;
    return src_x * DISPLAY_W / VIRTUAL_SCREEN_W;
}
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
//
// Sprite is 8bpp palette indices (typical SCUMM cursors are 16×16).  We
// always UPSCALE the sprite into LCD pixel space — never downsample —
// and we use a box-filter "any-source-pixel" merge so single-pixel
// features (cross-hair horizontal arm, arrow tip) survive regardless of
// the scale ratio.  Naive nearest-neighbour downsampling could drop a
// 1-row horizontal arm entirely when integer rounding happens to skip
// the row; that produced the "only vertical lines" report for Fill.
//
// Per-mode LCD size:
//   Fit  — 2× source pixels (a 16×16 cursor renders at 32×32 LCD).  Big
//          and bold against the 0.4× downsampled scene.
//   Fill — 1.5× source.
//   Crop — 1× source (1:1 native).
static void blit_cursor_overlay(uint16_t *fb, const CursorInfo &c,
                                const uint8_t *palette,
                                ScaleMode mode, int crop_x, int crop_y,
                                bool panel_active) {
    using namespace tsb::platform_pico;     // for pal_to_565
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
        // Clip to scene area — sentence strip below must not be touched.
        if (dy < 0 || dy >= kSceneLcdRows) continue;
        // Box-filter source span for this LCD row.  Always covers ≥1 row
        // (we never downsample), so vertical features can't be skipped.
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

            // Pick the first non-transparent pixel in the source span.
            // Equivalent to nearest-neighbour at 1:1, but recovers thin
            // strokes at any upscale ratio.
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

// THUMBY-PORT — paint a glyph stamp at the per-stamp scale (default 3:4
// = 75%, but the SPCH SCALE slider can drive it up to 100%/100% = 1:1)
// with an area-weighted blend against the framebuffer.  See
// platform_sdl.cpp for the design rationale; this is the device-side
// mirror.
static inline void blit_text_stamp(uint16_t *fb,
                                   const TextStamp &s,
                                   const uint8_t *palette) {
    using namespace tsb::platform_pico;     // for pal_to_565
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
    // setting, and every MI-overlay-font speech glyph (which is already
    // at LCD resolution).  Both SCUMM and MI glyphs go through the
    // same palette+cmap path: cmap[1] is whichever talk-colour palette
    // index the engine selected for the line, identical for both fonts.
    if (kNum == kDen || (s.flags & kTextStampFlagFullScale)) {
        for (int sy = 0; sy < s.height; sy++) {
            const int fb_y = s.dst_y + sy;
            if (fb_y < 0 || fb_y >= DISPLAY_H) continue;
            for (int sx = 0; sx < s.width; sx++) {
                const int fb_x = s.dst_x + sx;
                if (fb_x < 0 || fb_x >= DISPLAY_W) continue;
                const uint8_t c = glyph[sy * 32 + sx];
                if (c == 0) continue;
                const int idx = (c < 4) ? s.cmap[c] : 0;
                fb[fb_y * DISPLAY_W + fb_x] = pal_to_565(palette, idx);
            }
        }
        return;
    }

    const int dst_w = (s.width  * kNum + kDen - 1) / kDen;
    const int dst_h = (s.height * kNum + kDen - 1) / kDen;
    const int kWeightTotal = kDen * kDen;

    for (int dy = 0; dy < dst_h; dy++) {
        const int fb_y = s.dst_y + dy;
        if (fb_y < 0 || fb_y >= DISPLAY_H) continue;
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
    using namespace tsb::platform_pico;
    uint16_t *fb = g_fb;
    lcd_wait_idle();
    memset(fb, 0, sizeof(g_fb));
    const int src_y_max = panel_active ? kSceneSrcRows : VIRTUAL_SCREEN_H;

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

    // ---------- Scene region (LCD 0..119) ----------
    if (mode == ScaleMode::Fit || mode == ScaleMode::Fill) {
        const int fit_rows  = (src_y_max * DISPLAY_W) / VIRTUAL_SCREEN_W;
        const int fill_rows = (src_y_max * DISPLAY_H) / VIRTUAL_SCREEN_H;
        int dst_h           = (mode == ScaleMode::Fit) ? fit_rows : fill_rows;
        if (dst_h > kSceneLcdRows) dst_h = kSceneLcdRows;
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
        } else {  // Fit
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
    } else { // Crop — 1:1, scene 128×120
        if (crop_x < 0) crop_x = 0;
        if (crop_y < 0) crop_y = 0;
        const int crop_x_max = VIRTUAL_SCREEN_W - DISPLAY_W;
        const int crop_y_max = src_y_max - kSceneLcdRows;
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

    // ---------- Talk-area glyph stamps ----------
    for (int i = 0; i < text_stamp_count; i++) {
        const TextStamp &s = text_stamps[i];
        if (s.dst_y >= kSentenceLcdY) continue;
        blit_text_stamp(fb, s, palette);
    }

    // ---------- Cursor (clipped to scene) ----------
    if (cursor) blit_cursor_overlay(fb, *cursor, palette,
                                     mode, crop_x, crop_y, panel_active);

    // ---------- Cursor tooltip (auto-verb hover label) ----------
    if (cursor && cursor_tooltip && cursor_tooltip[0]) {
        const int anchor_x = src_to_lcd_x(cursor->x, mode, crop_x);
        const int anchor_y = src_to_lcd_y(cursor->y, mode, crop_y, panel_active);
        const int tw = tsb::mi_font::text_width(cursor_tooltip);
        constexpr uint16_t kTipColor = 0xCE2C;     // MI1 highlight #cec760
        int tx = anchor_x + 6;
        int ty = anchor_y + 4;
        if (tx + tw > DISPLAY_W - 1) tx = anchor_x - tw - 4;
        if (ty + 8 > kSentenceLcdY)  ty = anchor_y - 9;
        if (tx < 1) tx = 1;
        if (ty < 1) ty = 1;
        tsb::mi_font::draw(tx, ty, cursor_tooltip, kTipColor);
    }

    // ---------- Sentence strip (LCD 120..127) ----------
    for (int y = kSentenceLcdY; y < DISPLAY_H; y++) {
        for (int x = 0; x < DISPLAY_W; x++) fb[y * DISPLAY_W + x] = 0;
    }
    if (sentence && sentence[0]) {
        // MI1 sentence-line palette: blue throughout (#0099aa → 0x04D5).
        constexpr uint16_t kAccent = 0x04D5;
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

    if (send_to_lcd) lcd_present(fb);
}

bool poll_input(Input *out) {
    using namespace tsb::platform_pico;

    struct buttons_state st{};
    buttons_read(&st);

    const uint32_t now_ms = (uint32_t)millis();
    debounce_step(g_db_a,     st.a,    now_ms);
    debounce_step(g_db_b,     st.b,    now_ms);
    debounce_step(g_db_lb,    st.lb,   now_ms);
    debounce_step(g_db_rb,    st.rb,   now_ms);
    debounce_step(g_db_menu,  st.menu, now_ms);
    debounce_step(g_db_up,    st.up,    now_ms);
    debounce_step(g_db_down,  st.down,  now_ms);
    debounce_step(g_db_left,  st.left,  now_ms);
    debounce_step(g_db_right, st.right, now_ms);

    out->dpad_up    = g_db_up.stable;
    out->dpad_down  = g_db_down.stable;
    out->dpad_left  = g_db_left.stable;
    out->dpad_right = g_db_right.stable;

    out->button_a    = g_db_a.stable;
    out->button_b    = g_db_b.stable;
    out->button_lb   = g_db_lb.stable;
    out->button_rb   = g_db_rb.stable;
    out->button_menu = g_db_menu.stable;

    out->a_pressed    =  g_db_a.stable    && !g_db_a.prev_stable;
    out->b_pressed    =  g_db_b.stable    && !g_db_b.prev_stable;
    out->lb_pressed   =  g_db_lb.stable   && !g_db_lb.prev_stable;
    out->rb_pressed   =  g_db_rb.stable   && !g_db_rb.prev_stable;
    out->menu_pressed =  g_db_menu.stable && !g_db_menu.prev_stable;

    out->a_released    = !g_db_a.stable    &&  g_db_a.prev_stable;
    out->b_released    = !g_db_b.stable    &&  g_db_b.prev_stable;
    out->lb_released   = !g_db_lb.stable   &&  g_db_lb.prev_stable;
    out->rb_released   = !g_db_rb.stable   &&  g_db_rb.prev_stable;
    out->menu_released = !g_db_menu.stable &&  g_db_menu.prev_stable;

    g_db_a.prev_stable    = g_db_a.stable;
    g_db_b.prev_stable    = g_db_b.stable;
    g_db_lb.prev_stable   = g_db_lb.stable;
    g_db_rb.prev_stable   = g_db_rb.stable;
    g_db_menu.prev_stable = g_db_menu.stable;
    g_db_up.prev_stable    = g_db_up.stable;
    g_db_down.prev_stable  = g_db_down.stable;
    g_db_left.prev_stable  = g_db_left.stable;
    g_db_right.prev_stable = g_db_right.stable;

    return true;  // device never quits
}

int audio_init(int requested_rate, AudioCallback cb, void *user) {
    using namespace tsb::platform_pico;
    g_audio_cb   = cb;
    g_audio_user = user;
    g_audio_rate = 22050;  // PWM timer is hardcoded to 22050 Hz
    // Defer the IRQ enable to here — at this point the mixer callback
    // is wired up and dbopl/imuse are ready. Until now the IRQ was
    // *registered* but disabled so it can't interrupt earlier engine
    // init. After this call, the IRQ starts draining the ring at 22050
    // Hz; engine_tick → audio_pump() refills it once per frame.
    audio_pwm_irq_enable();
    return g_audio_rate;
}

// Refill the PWM ring with up to one frame's worth of new samples.
// Called from engine_tick once per frame on core 0. Mixer callback runs
// here too, so iMUSE state mutation by the engine and reads by the
// callback never overlap — they're both sequential on core 0.
void audio_pump() {
    using namespace tsb::platform_pico;
    if (!g_audio_cb) return;
    int room = audio_pwm_room();
    while (room >= 64) {                       // top up in 64-sample chunks
        int n = room < kMixChunkSamples ? room : kMixChunkSamples;
        g_audio_cb(g_audio_user, s_mix_buf, n);
        audio_pwm_push(s_mix_buf, n);
        room -= n;
    }
}

void audio_shutdown() {
    using namespace tsb::platform_pico;
    g_audio_cb = nullptr;
}

uint32_t millis() {
    return (uint32_t)(time_us_64() / 1000ull);
}
uint32_t micros() {
    return (uint32_t)time_us_64();
}
void sleep_ms(uint32_t ms) {
    ::sleep_ms(ms);
}

static void logAppendChar(char c);  // forward decl
static void logRenderToFb();        // forward decl
// Survive-reset SRAM log ring. Lives in uninitialized SRAM so the
// contents persist across watchdog/soft reset (RP2350 only loses
// non-NoInit SRAM on full power cycle). After a hang, hold MENU on the
// next boot to view this ring before engine init runs.
#define TSB_LOG_RING_MAGIC 0xCAFE1234u
#define TSB_LOG_RING_SIZE  4096
struct LogRing {
    uint32_t magic;
    uint32_t head;
    uint32_t wrapped;
    char     buf[TSB_LOG_RING_SIZE];
};
__attribute__((section(".uninitialized_data")))
static LogRing g_logRing;

static void logRingReset() {
    g_logRing.magic   = TSB_LOG_RING_MAGIC;
    g_logRing.head    = 0;
    g_logRing.wrapped = 0;
    for (int i = 0; i < TSB_LOG_RING_SIZE; i++) g_logRing.buf[i] = 0;
}
static inline void logRingEnsureValid() {
    // First boot after power-on: SRAM is random garbage, magic almost
    // certainly doesn't match. Initialise.
    if (g_logRing.magic != TSB_LOG_RING_MAGIC) logRingReset();
}
static void logRingAppend(const char *s, int n) {
    logRingEnsureValid();
    for (int i = 0; i < n; i++) {
        g_logRing.buf[g_logRing.head] = s[i];
        g_logRing.head = (g_logRing.head + 1) % TSB_LOG_RING_SIZE;
        if (g_logRing.head == 0) g_logRing.wrapped = 1;
    }
}

void log(const char *fmt, ...) {
    char buf[128];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    // Persist to the survive-reset ring first (cheap, no LCD I/O).
    logRingAppend(buf, n);
    // Mirror to bottom-of-screen FB overlay (kept for boot-time visibility
    // before the engine starts painting full-screen frames).
    for (int i = 0; i < n; i++) logAppendChar(buf[i]);
}
void log_flush() { logRenderToFb(); }

size_t heap_free() {
    struct mallinfo mi = mallinfo();
    return (size_t)mi.fordblks;
}
size_t heap_used() {
    struct mallinfo mi = mallinfo();
    return (size_t)mi.uordblks;
}

// log_show_previous_boot defined further down — body lives after
// kFont5x7's definition so name lookup resolves.

// Boot diagnostic splash — paints a 128×128 solid frame so a hang
// inside engine_init can be localised by the colour on screen.
void debug_splash(uint16_t rgb565) {
    using namespace tsb::platform_pico;
    for (int i = 0; i < DISPLAY_W * DISPLAY_H; i++) g_fb[i] = rgb565;
    lcd_present(g_fb);
    lcd_wait_idle();
    sleep_ms(80);
}

// ---------------------------------------------------------------------------
// On-screen 5x7 ASCII font + console-style log overlay
// ---------------------------------------------------------------------------
// Public-domain 5x7 font for printable ASCII (0x20..0x7E). Each glyph is
// 5 bytes; bit i of byte j is the pixel at column j, row i (top-down).
// Stored in flash via the const qualifier — zero RAM cost.
static const uint8_t kFont5x7[95][5] = {
    {0,0,0,0,0},          // 0x20 ' '
    {0,0,0x5F,0,0},       // !
    {0,7,0,7,0},          // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,8,0x64,0x62},    // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0,5,3,0,0},          // '
    {0,0x1C,0x22,0x41,0}, // (
    {0,0x41,0x22,0x1C,0}, // )
    {0x14,8,0x3E,8,0x14}, // *
    {8,8,0x3E,8,8},       // +
    {0,0x50,0x30,0,0},    // ,
    {8,8,8,8,8},          // -
    {0,0x60,0x60,0,0},    // .
    {0x20,0x10,8,4,2},    // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0,0x42,0x7F,0x40,0}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {1,0x71,9,5,3},       // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {6,0x49,0x49,0x29,0x1E}, // 9
    {0,0x36,0x36,0,0},    // :
    {0,0x56,0x36,0,0},    // ;
    {0,8,0x14,0x22,0x41}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x41,0x22,0x14,8,0}, // >
    {2,1,0x51,9,6},       // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,9,9,1,1},       // F
    {0x3E,0x41,0x41,0x51,0x32}, // G
    {0x7F,8,8,8,0x7F},    // H
    {0,0x41,0x7F,0x41,0}, // I
    {0x20,0x40,0x41,0x3F,1}, // J
    {0x7F,8,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,2,4,2,0x7F},    // M
    {0x7F,4,8,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,9,9,9,6},       // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,9,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {1,1,0x7F,1,1},       // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x7F,0x20,0x18,0x20,0x7F}, // W
    {0x63,0x14,8,0x14,0x63}, // X
    {3,4,0x78,4,3},       // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0,0x7F,0x41,0x41,0}, // [
    {2,4,8,0x10,0x20},    // backslash
    {0,0x41,0x41,0x7F,0}, // ]
    {4,2,1,2,4},          // ^
    {0x40,0x40,0x40,0x40,0x40}, // _
    {0,1,2,4,0},          // `
    {0x20,0x54,0x54,0x54,0x78}, // a
    {0x7F,0x48,0x44,0x44,0x38}, // b
    {0x38,0x44,0x44,0x44,0x20}, // c
    {0x38,0x44,0x44,0x48,0x7F}, // d
    {0x38,0x54,0x54,0x54,0x18}, // e
    {8,0x7E,9,1,2},       // f
    {0x08,0x14,0x54,0x54,0x3C}, // g
    {0x7F,8,4,4,0x78},    // h
    {0,0x44,0x7D,0x40,0}, // i
    {0x20,0x40,0x44,0x3D,0}, // j
    {0x7F,0x10,0x28,0x44,0}, // k
    {0,0x41,0x7F,0x40,0}, // l
    {0x7C,4,0x18,4,0x78}, // m
    {0x7C,8,4,4,0x78},    // n
    {0x38,0x44,0x44,0x44,0x38}, // o
    {0x7C,0x14,0x14,0x14,8}, // p
    {8,0x14,0x14,0x18,0x7C}, // q
    {0x7C,8,4,4,8},       // r
    {0x48,0x54,0x54,0x54,0x20}, // s
    {4,0x3F,0x44,0x40,0x20}, // t
    {0x3C,0x40,0x40,0x20,0x7C}, // u
    {0x1C,0x20,0x40,0x20,0x1C}, // v
    {0x3C,0x40,0x30,0x40,0x3C}, // w
    {0x44,0x28,0x10,0x28,0x44}, // x
    {0x0C,0x50,0x50,0x50,0x3C}, // y
    {0x44,0x64,0x54,0x4C,0x44}, // z
    {0,8,0x36,0x41,0},    // {
    {0,0,0x7F,0,0},       // |
    {0,0x41,0x36,8,0},    // }
    {8,4,8,0x10,8},       // ~
};

// 8 lines × 21 chars (uses 6 px per char, fits 128 / 6 = 21).
// 15 lines × 8 px = 120 px, + 8 px header = full 128 px LCD coverage.
// User reads these directly off the LCD during boot — no reset needed.
static constexpr int kLogLines = 15;
static constexpr int kLogCols  = 21;
static char     g_logBuf[kLogLines][kLogCols + 1] = {{0}};
static int      g_logCursor = 0;  // current line being filled
static int      g_logColCur = 0;  // current column in current line

static void logRenderToFb() {
    using namespace tsb::platform_pico;
    // Clear bottom 8*8=64 px of LCD framebuffer (lines 64..127).
    const int top = DISPLAY_H - kLogLines * 8;
    for (int y = top; y < DISPLAY_H; y++)
        for (int x = 0; x < DISPLAY_W; x++)
            g_fb[y * DISPLAY_W + x] = 0x0000;  // black bg
    // Render each log line top-down, oldest line at top.
    // Buffer is filled circularly: g_logCursor points to NEXT line to write.
    // Display oldest first: cursor, cursor+1, ..., cursor-1 (mod kLogLines).
    for (int row = 0; row < kLogLines; row++) {
        int srcLine = (g_logCursor + row) % kLogLines;
        const char *txt = g_logBuf[srcLine];
        int py = top + row * 8;
        for (int col = 0; col < kLogCols && txt[col]; col++) {
            char c = txt[col];
            if (c < 0x20 || c > 0x7E) c = '?';
            const uint8_t *glyph = kFont5x7[c - 0x20];
            int px = col * 6;
            for (int gx = 0; gx < 5; gx++) {
                uint8_t bits = glyph[gx];
                for (int gy = 0; gy < 7; gy++) {
                    if (bits & (1 << gy)) {
                        g_fb[(py + gy) * DISPLAY_W + (px + gx)] = 0xFFFF;
                    }
                }
            }
        }
    }
    lcd_present(g_fb);
    lcd_wait_idle();
}

// Render every newline. Slower (~5-10 ms per LCD frame DMA) but
// guarantees we see the very last log line before any hang.
static void logAppendChar(char c) {
    if (c == '\n') {
        g_logBuf[g_logCursor][g_logColCur] = '\0';
        g_logCursor = (g_logCursor + 1) % kLogLines;
        g_logColCur = 0;
        memset(g_logBuf[g_logCursor], 0, sizeof(g_logBuf[g_logCursor]));
        logRenderToFb();
    } else if (g_logColCur < kLogCols) {
        g_logBuf[g_logCursor][g_logColCur++] = c;
    }
}

void checkpoint(const char *label, uint16_t /*color*/) {
    // On device, render the label as a log line. Color ignored.
    if (label) {
        for (const char *p = label; *p; p++) logAppendChar(*p);
        logAppendChar('\n');
    }
}

[[noreturn]] void panic(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("PANIC: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    // Stop interrupts and spin. Operator's only recourse is power-cycle.
    __asm volatile("cpsid i");
    while (1) { __asm volatile("wfi"); }
}

// ---------------------------------------------------------------------------
// Save/Load menu primitives — direct LCD framebuffer paint.
// ---------------------------------------------------------------------------

void lcd_fill(uint16_t rgb565) {
    using namespace tsb::platform_pico;
    for (int i = 0; i < DISPLAY_W * DISPLAY_H; i++) {
        g_fb[i] = rgb565;
    }
}

void lcd_pixel(int x, int y, uint16_t rgb565) {
    using namespace tsb::platform_pico;
    if ((unsigned)x >= (unsigned)DISPLAY_W) return;
    if ((unsigned)y >= (unsigned)DISPLAY_H) return;
    g_fb[y * DISPLAY_W + x] = rgb565;
}

void lcd_present_now() {
    using namespace tsb::platform_pico;
    lcd_present(g_fb);
    lcd_wait_idle();
}

bool is_lb_held() {
    struct buttons_state st;
    buttons_read(&st);
    return st.lb;
}

bool is_rb_held() {
    struct buttons_state st;
    buttons_read(&st);
    return st.rb;
}

bool is_menu_held() {
    struct buttons_state st;
    buttons_read(&st);
    return st.menu;
}

void lcd_dim_box(int x, int y, int w, int h) {
    using namespace tsb::platform_pico;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_W) w = DISPLAY_W - x;
    if (y + h > DISPLAY_H) h = DISPLAY_H - y;
    if (w <= 0 || h <= 0) return;
    for (int dy = 0; dy < h; dy++) {
        uint16_t *row = g_fb + (y + dy) * DISPLAY_W + x;
        for (int dx = 0; dx < w; dx++) {
            row[dx] = (row[dx] >> 1) & 0x7BEFu;
        }
    }
}

// Render previous boot's log ring to LCD with paging, then clear it
// for the new boot. Called from main() between platform_init and
// engine bringup. If ring is empty or magic invalid, returns
// immediately (first boot after power-on, or no prior log).
//
// Controls:
//   UP/DOWN  scroll one line
//   LB/RB    scroll one page
//   A        dismiss, continue to engine
void log_show_previous_boot() {
    using namespace tsb::platform_pico;
    if (g_logRing.magic != TSB_LOG_RING_MAGIC) {
        logRingReset();
        return;
    }
    if (g_logRing.head == 0 && !g_logRing.wrapped) return;

    // Linearise the ring (oldest -> newest) into a temp buffer.
    static char lin[TSB_LOG_RING_SIZE + 1];
    size_t total = 0;
    if (g_logRing.wrapped) {
        for (uint32_t i = g_logRing.head; i < TSB_LOG_RING_SIZE; i++)
            lin[total++] = g_logRing.buf[i];
    }
    for (uint32_t i = 0; i < g_logRing.head; i++)
        lin[total++] = g_logRing.buf[i];
    lin[total] = 0;

    // Index lines, wrapping at 21 chars OR newline so nothing in the
    // ring gets truncated on screen.
    constexpr int kMaxLines = 512;
    constexpr int kRowChars = 21;
    static const char *line_ptr[kMaxLines];
    static int         line_len[kMaxLines];
    int line_count = 0;
    {
        size_t pos = 0;
        while (pos < total && line_count < kMaxLines) {
            size_t row_start = pos;
            int row_chars = 0;
            while (pos < total && lin[pos] != '\n' && row_chars < kRowChars) {
                pos++;
                row_chars++;
            }
            line_ptr[line_count] = lin + row_start;
            line_len[line_count] = row_chars;
            line_count++;
            if (pos < total && lin[pos] == '\n') pos++;
        }
    }
    if (line_count == 0) { logRingReset(); return; }

    const int rows_visible = 15;
    int scroll = (line_count > rows_visible) ? (line_count - rows_visible) : 0;

    auto draw_glyph = [](int px, int py, char c, uint16_t fg) {
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *glyph = kFont5x7[c - 0x20];
        for (int gx = 0; gx < 5; gx++) {
            uint8_t bits = glyph[gx];
            for (int gy = 0; gy < 7; gy++) {
                if (bits & (1 << gy)) {
                    int x = px + gx, y = py + gy;
                    if (x >= 0 && x < DISPLAY_W && y >= 0 && y < DISPLAY_H)
                        g_fb[y * DISPLAY_W + x] = fg;
                }
            }
        }
    };

    auto render = [&]() {
        for (int i = 0; i < DISPLAY_W * DISPLAY_H; i++) g_fb[i] = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < DISPLAY_W; x++)
                g_fb[y * DISPLAY_W + x] = 0xF800;
        const char hdr[] = "BOOT LOG A=GO U/D=SCRL";
        for (int col = 0; col < 21 && hdr[col]; col++)
            draw_glyph(col * 6, 0, hdr[col], 0xFFFF);
        for (int row = 0; row < rows_visible; row++) {
            int idx = scroll + row;
            if (idx >= line_count) break;
            int py = 8 + row * 8;
            const char *txt = line_ptr[idx];
            int len = line_len[idx];
            for (int col = 0; col < 21 && col < len; col++)
                draw_glyph(col * 6, py, txt[col], 0xFFFF);
        }
        lcd_present(g_fb);
        lcd_wait_idle();
    };

    struct buttons_state prev = {}, bs = {};
    buttons_read(&prev);
    render();
    while (true) {
        sleep_ms(30);
        buttons_read(&bs);
        bool dirty = false;
        if (bs.a && !prev.a) break;
        if (bs.up && !prev.up) {
            if (scroll > 0) { scroll--; dirty = true; }
        }
        if (bs.down && !prev.down) {
            if (scroll + rows_visible < line_count) { scroll++; dirty = true; }
        }
        if (bs.lb && !prev.lb) {
            scroll -= rows_visible - 1;
            if (scroll < 0) scroll = 0;
            dirty = true;
        }
        if (bs.rb && !prev.rb) {
            scroll += rows_visible - 1;
            int max = line_count - rows_visible;
            if (max < 0) max = 0;
            if (scroll > max) scroll = max;
            dirty = true;
        }
        prev = bs;
        if (dirty) render();
    }
    logRingReset();
}

}  // namespace tsb::platform

// ---------------------------------------------------------------------------
// One-shot platform init called from main() before engine_init().
// ---------------------------------------------------------------------------
namespace tsb::platform_pico {

void init_all() {
    parse_blob();
    lcd_init();
    buttons_init();
    audio_pwm_init();
}

bool blob_ok() { return g_blob_ok; }

}  // namespace tsb::platform_pico
