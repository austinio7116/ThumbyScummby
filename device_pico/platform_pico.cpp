// ThumbyScummby — Pico SDK implementation of engine/include/platform.h.
//
// Bare-metal Thumby Color (RP2350) target. Game data .incbin'd in flash,
// 128x128 RGB565 LCD via GC9107 + DMA, GPIO buttons, dual-core PWM audio
// (core1 mixer feeds the PWM ring fed by an IRQ-driven 22050 Hz timer).

#include "platform.h"
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
// Input edge detection (mirrors the host_sdl path)
// ---------------------------------------------------------------------------
static struct {
    bool prev_a, prev_b, prev_lb, prev_rb, prev_menu;
} g_input_state;

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

// Source-coord → LCD-coord helpers, matching the present() scaling math.
// Used both for the main blit AND for blit_cursor_overlay so the cursor
// position lands exactly under the user's logical mouse coord.
static inline int src_to_lcd_x(int src_x, ScaleMode mode, int crop_x) {
    if (mode == ScaleMode::Fill)
        return (src_x - crop_x) * DISPLAY_H / VIRTUAL_SCREEN_H;
    if (mode == ScaleMode::Crop)
        return src_x - crop_x;
    return src_x * DISPLAY_W / VIRTUAL_SCREEN_W;
}
static inline int src_to_lcd_y(int src_y, ScaleMode mode, int crop_y) {
    if (mode == ScaleMode::Crop) return src_y - crop_y;
    const int dst_h = (mode == ScaleMode::Fill) ? DISPLAY_H : 80;
    const int top   = (DISPLAY_H - dst_h) / 2;
    return top + src_y * dst_h / VIRTUAL_SCREEN_H;
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
                                ScaleMode mode, int crop_x, int crop_y) {
    using namespace tsb::platform_pico;     // for pal_to_565
    if (!c.sprite || c.w <= 0 || c.h <= 0) return;

    // Per-mode LCD cursor size:
    //   Fit  — 1.25× source (20 LCD px for a 16-px source).
    //   Fill — 1.125× source (18 LCD px).
    //   Crop — 1× source (16 LCD px, 1:1 native).
    int cw_lcd, ch_lcd;
    if (mode == ScaleMode::Fit) {
        cw_lcd = c.w * 5 / 4;
        ch_lcd = c.h * 5 / 4;
    } else if (mode == ScaleMode::Fill) {
        cw_lcd = c.w * 9 / 8;
        ch_lcd = c.h * 9 / 8;
    } else {  // Crop
        cw_lcd = c.w;
        ch_lcd = c.h;
    }
    if (cw_lcd < 4) cw_lcd = 4;
    if (ch_lcd < 4) ch_lcd = 4;

    const int anchor_lcd_x = src_to_lcd_x(c.x, mode, crop_x);
    const int anchor_lcd_y = src_to_lcd_y(c.y, mode, crop_y);
    const int hsx_lcd = c.hotspot_x * cw_lcd / c.w;
    const int hsy_lcd = c.hotspot_y * ch_lcd / c.h;
    const int origin_x = anchor_lcd_x - hsx_lcd;
    const int origin_y = anchor_lcd_y - hsy_lcd;

    for (int ly = 0; ly < ch_lcd; ly++) {
        const int dy = origin_y + ly;
        if (dy < 0 || dy >= DISPLAY_H) continue;
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

void present(const uint8_t *virt, const uint8_t *text,
             const uint8_t *palette,
             ScaleMode mode, int crop_x, int crop_y,
             const CursorInfo *cursor) {
    using namespace tsb::platform_pico;
    uint16_t *fb = g_fb;

    // The previous DMA push must finish before we touch g_fb.
    lcd_wait_idle();

    if (mode == ScaleMode::Fit || mode == ScaleMode::Fill) {
        const int dst_h = (mode == ScaleMode::Fill) ? DISPLAY_H : 80;
        const int letterbox_top = (DISPLAY_H - dst_h) / 2;
        if (letterbox_top > 0) memset(fb, 0, sizeof(g_fb));

        // Per-dx source X pair (sx, sx2 = sx+1 clamped). FIT maps the
        // 320 width onto 128 dst pixels (anisotropic). FILL keeps the
        // vertical scale ratio in X (isotropic, 200→128) and pans
        // horizontally via crop_x in source-x pixels (range 0..120).
        uint16_t sxa[DISPLAY_W], sxb[DISPLAY_W];
        if (mode == ScaleMode::Fill) {
            int pan_max = VIRTUAL_SCREEN_W -
                          (DISPLAY_W * VIRTUAL_SCREEN_H / DISPLAY_H);
            if (crop_x < 0)        crop_x = 0;
            if (crop_x > pan_max)  crop_x = pan_max;
            for (int dx = 0; dx < DISPLAY_W; dx++) {
                int sx  = crop_x + (dx * VIRTUAL_SCREEN_H) / DISPLAY_H;
                int sx2 = sx + 1; if (sx2 >= VIRTUAL_SCREEN_W) sx2 = sx;
                sxa[dx] = (uint16_t)sx;
                sxb[dx] = (uint16_t)sx2;
            }
        } else {
            for (int dx = 0; dx < DISPLAY_W; dx++) {
                int sx  = (dx * VIRTUAL_SCREEN_W) / DISPLAY_W;
                int sx2 = sx + 1; if (sx2 >= VIRTUAL_SCREEN_W) sx2 = sx;
                sxa[dx] = (uint16_t)sx;
                sxb[dx] = (uint16_t)sx2;
            }
        }

        for (int dy = 0; dy < dst_h; dy++) {
            int sy  = (dy * VIRTUAL_SCREEN_H) / dst_h;
            int sy2 = sy + 1; if (sy2 >= VIRTUAL_SCREEN_H) sy2 = sy;
            const uint8_t *vrow1 = virt + sy  * VIRTUAL_SCREEN_W;
            const uint8_t *vrow2 = virt + sy2 * VIRTUAL_SCREEN_W;
            const uint8_t *trow1 = text ? text + sy  * VIRTUAL_SCREEN_W : nullptr;
            const uint8_t *trow2 = text ? text + sy2 * VIRTUAL_SCREEN_W : nullptr;
            uint16_t *drow = fb + (dy + letterbox_top) * DISPLAY_W;
            for (int dx = 0; dx < DISPLAY_W; dx++) {
                int sx = sxa[dx], sx2 = sxb[dx];
                // Text composited per source pixel BEFORE the 2x2 box
                // blend, so glyph edges anti-alias against the
                // background. Trade-off: 1-pixel strokes soften, but
                // overall readability improves at 320->128 downsample.
                uint8_t s_a = trow1 ? resolve_src(trow1[sx],  vrow1[sx])  : vrow1[sx];
                uint8_t s_b = trow1 ? resolve_src(trow1[sx2], vrow1[sx2]) : vrow1[sx2];
                uint8_t s_c = trow2 ? resolve_src(trow2[sx],  vrow2[sx])  : vrow2[sx];
                uint8_t s_d = trow2 ? resolve_src(trow2[sx2], vrow2[sx2]) : vrow2[sx2];
                uint16_t pa = pal_to_565(palette, s_a);
                uint16_t pb = pal_to_565(palette, s_b);
                uint16_t pc = pal_to_565(palette, s_c);
                uint16_t pd = pal_to_565(palette, s_d);
                drow[dx] = blend4_565(pa, pb, pc, pd);
            }
        }
    } else { // Crop — 1:1 native, pannable
        if (crop_x < 0) crop_x = 0;
        if (crop_y < 0) crop_y = 0;
        if (crop_x > VIRTUAL_SCREEN_W - DISPLAY_W) crop_x = VIRTUAL_SCREEN_W - DISPLAY_W;
        if (crop_y > VIRTUAL_SCREEN_H - DISPLAY_H) crop_y = VIRTUAL_SCREEN_H - DISPLAY_H;
        for (int dy = 0; dy < DISPLAY_H; dy++) {
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

    if (cursor) blit_cursor_overlay(fb, *cursor, palette, mode, crop_x, crop_y);

    lcd_present(fb);
}

bool poll_input(Input *out) {
    using namespace tsb::platform_pico;

    struct buttons_state st{};
    buttons_read(&st);

    out->dpad_up    = st.up;
    out->dpad_down  = st.down;
    out->dpad_left  = st.left;
    out->dpad_right = st.right;

    out->button_a    = st.a;
    out->button_b    = st.b;
    out->button_lb   = st.lb;
    out->button_rb   = st.rb;
    out->button_menu = st.menu;

    out->a_pressed    = st.a    && !g_input_state.prev_a;
    out->b_pressed    = st.b    && !g_input_state.prev_b;
    out->lb_pressed   = st.lb   && !g_input_state.prev_lb;
    out->rb_pressed   = st.rb   && !g_input_state.prev_rb;
    out->menu_pressed = st.menu && !g_input_state.prev_menu;

    out->a_released    = !st.a    && g_input_state.prev_a;
    out->b_released    = !st.b    && g_input_state.prev_b;
    out->lb_released   = !st.lb   && g_input_state.prev_lb;
    out->rb_released   = !st.rb   && g_input_state.prev_rb;
    out->menu_released = !st.menu && g_input_state.prev_menu;

    g_input_state.prev_a    = st.a;
    g_input_state.prev_b    = st.b;
    g_input_state.prev_lb   = st.lb;
    g_input_state.prev_rb   = st.rb;
    g_input_state.prev_menu = st.menu;

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
void log(const char *fmt, ...) {
    char buf[128];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    for (int i = 0; i < n; i++) logAppendChar(buf[i]);
}
void log_flush() { logRenderToFb(); }

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
static constexpr int kLogLines = 8;
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
