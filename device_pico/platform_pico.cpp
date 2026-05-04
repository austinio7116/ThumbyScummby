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

// Pick most-visible text sample from up-to-4 candidates.
// Priority: foreground (non-FD non-zero) > shadow (zero) > transparent (FD).
// 0xFD is scummvm's CHARSET_MASK_TRANSPARENCY (gfx.h:289).
static inline uint8_t text_pick4(uint8_t a, uint8_t b,
                                 uint8_t c, uint8_t d) {
    uint8_t fg = 0xFD;
    bool has_shadow = false;
    if (a != 0xFD) { if (a) { if (fg == 0xFD) fg = a; } else has_shadow = true; }
    if (b != 0xFD) { if (b) { if (fg == 0xFD) fg = b; } else has_shadow = true; }
    if (c != 0xFD) { if (c) { if (fg == 0xFD) fg = c; } else has_shadow = true; }
    if (d != 0xFD) { if (d) { if (fg == 0xFD) fg = d; } else has_shadow = true; }
    if (fg != 0xFD) return fg;
    if (has_shadow) return 0;
    return 0xFD;
}

void present(const uint8_t *virt, const uint8_t *text,
             const uint8_t *palette,
             ScaleMode mode, int crop_x, int crop_y) {
    using namespace tsb::platform_pico;
    uint16_t *fb = g_fb;

    // The previous DMA push must finish before we touch g_fb.
    lcd_wait_idle();

    if (mode == ScaleMode::Fit || mode == ScaleMode::Fill) {
        const int dst_h = (mode == ScaleMode::Fill) ? DISPLAY_H : 80;
        const int letterbox_top = (DISPLAY_H - dst_h) / 2;
        if (letterbox_top > 0) memset(fb, 0, sizeof(g_fb));

        // Per-dx source X pair (sx, sx2 = sx+1 clamped). Mirrors
        // md_core_rebuild_sx_lut at md_core.c:514-519.
        uint16_t sxa[DISPLAY_W], sxb[DISPLAY_W];
        for (int dx = 0; dx < DISPLAY_W; dx++) {
            int sx  = (dx * VIRTUAL_SCREEN_W) / DISPLAY_W;
            int sx2 = sx + 1; if (sx2 >= VIRTUAL_SCREEN_W) sx2 = sx;
            sxa[dx] = (uint16_t)sx;
            sxb[dx] = (uint16_t)sx2;
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
                uint8_t tpick = 0xFD;
                if (trow1) {
                    tpick = text_pick4(trow1[sx], trow1[sx2],
                                       trow2[sx], trow2[sx2]);
                }
                if (tpick != 0xFD) {
                    drow[dx] = pal_to_565(palette, tpick);
                } else {
                    uint16_t pa = pal_to_565(palette, vrow1[sx]);
                    uint16_t pb = pal_to_565(palette, vrow1[sx2]);
                    uint16_t pc = pal_to_565(palette, vrow2[sx]);
                    uint16_t pd = pal_to_565(palette, vrow2[sx2]);
                    drow[dx] = blend4_565(pa, pb, pc, pd);
                }
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

void log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

// Boot diagnostic splash — paints a 128×128 solid frame so a hang
// inside engine_init can be localised by the colour on screen.
void debug_splash(uint16_t rgb565) {
    using namespace tsb::platform_pico;
    // Reuse g_fb (the present-time framebuffer) so we don't burn an
    // extra 32KB BSS for diagnostics.
    for (int i = 0; i < DISPLAY_W * DISPLAY_H; i++) g_fb[i] = rgb565;
    lcd_present(g_fb);
    lcd_wait_idle();
    // Short hold — only need long enough that human eye registers the
    // colour change. The LAST splash before a hang stays on screen
    // forever, so 80ms is fine for sequencing.
    sleep_ms(80);
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
