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
#include "pico/multicore.h"
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
// Audio: core1 runs a mixer loop that pulls samples from the engine's
// platform-supplied callback and pushes them into the PWM ring.
// ---------------------------------------------------------------------------

static tsb::platform::AudioCallback g_audio_cb = nullptr;
static void                        *g_audio_user = nullptr;
static int                          g_audio_rate = 22050;

// Core1 mix buffer. 256 samples @ 22050 Hz = ~11.6 ms per buffer; PWM ring
// is 4096 samples = ~186 ms of slack, so this is generous.
constexpr int kMixChunkSamples = 256;

static void __not_in_flash_func(core1_audio_loop)(void) {
    multicore_lockout_victim_init();   // allow core0 to lock us out

    int16_t buf[kMixChunkSamples];
    while (1) {
        if (g_audio_cb && audio_pwm_room() >= kMixChunkSamples) {
            g_audio_cb(g_audio_user, buf, kMixChunkSamples);
            audio_pwm_push(buf, kMixChunkSamples);
        } else {
            tight_loop_contents();
        }
    }
}

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

void present(const uint8_t *virt, const uint8_t *palette,
             ScaleMode mode, int crop_x, int crop_y) {
    using namespace tsb::platform_pico;
    uint16_t *fb = g_fb;

    // The previous DMA push must finish before we touch g_fb.
    lcd_wait_idle();

    if (mode == ScaleMode::Fit) {
        // 320x200 -> 128x80 letterboxed (24 px black bars top + bottom)
        memset(fb, 0, sizeof(g_fb));
        for (int dy = 0; dy < 80; dy++) {
            int sy = (dy * 5) >> 1;            // 0..199
            const uint8_t *srow = virt + sy * VIRTUAL_SCREEN_W;
            uint16_t *drow = fb + (dy + 24) * DISPLAY_W;
            for (int dx = 0; dx < DISPLAY_W; dx++) {
                int sx = (dx * 5) >> 1;        // 0..319
                drow[dx] = pal_to_565(palette, srow[sx]);
            }
        }
    } else if (mode == ScaleMode::Fill) {
        // 320x200 -> 128x128 anisotropic
        for (int dy = 0; dy < DISPLAY_H; dy++) {
            int sy = (dy * 25) >> 4;
            const uint8_t *srow = virt + sy * VIRTUAL_SCREEN_W;
            uint16_t *drow = fb + dy * DISPLAY_W;
            for (int dx = 0; dx < DISPLAY_W; dx++) {
                int sx = (dx * 5) >> 1;
                drow[dx] = pal_to_565(palette, srow[sx]);
            }
        }
    } else { // Crop
        if (crop_x < 0) crop_x = 0;
        if (crop_y < 0) crop_y = 0;
        if (crop_x > VIRTUAL_SCREEN_W - DISPLAY_W) crop_x = VIRTUAL_SCREEN_W - DISPLAY_W;
        if (crop_y > VIRTUAL_SCREEN_H - DISPLAY_H) crop_y = VIRTUAL_SCREEN_H - DISPLAY_H;
        for (int dy = 0; dy < DISPLAY_H; dy++) {
            const uint8_t *srow = virt + (crop_y + dy) * VIRTUAL_SCREEN_W + crop_x;
            uint16_t *drow = fb + dy * DISPLAY_W;
            for (int dx = 0; dx < DISPLAY_W; dx++) {
                drow[dx] = pal_to_565(palette, srow[dx]);
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
    multicore_launch_core1(core1_audio_loop);
    return g_audio_rate;
}

void audio_shutdown() {
    using namespace tsb::platform_pico;
    g_audio_cb = nullptr;
    // Core1 keeps spinning but produces silence (memset of buf, but we
    // skip when g_audio_cb is null).
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
