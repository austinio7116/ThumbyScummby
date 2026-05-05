// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — platform abstraction layer.
//
// All hardware/OS access by the engine goes through these functions.
// Host SDL build implements them via SDL2; device build implements them
// via Pico SDK. Engine code links against this header only — never SDL,
// never Pico SDK, never libc beyond <stdint.h>/<string.h>.

#pragma once

#include "types.h"

namespace tsb::platform {

// ---------------------------------------------------------------------------
// File access (game data)
// ---------------------------------------------------------------------------
//
// On host: files are mmap'd from disk on engine init.
// On device: files are concatenated in flash via .incbin; this returns a
// pointer into XIP'd flash. Either way, the returned span is read-only and
// stable until shutdown. NEVER memcpy into SRAM in bulk — read fields
// in-place from the span.
//
// The decryption (0x69 XOR) happens at host load time (mmap copy-on-read)
// or at firmware build time (host-side script decrypts before .incbin).
// The view returned here is ALREADY DECRYPTED.

// Get the master index file. Returns empty span if not loaded.
Span data_master_index();          // 000.LFL (decrypted)

// Get a disk-image LEC. disk_id is 1..4.
Span data_disk(int disk_id);       // DISK0n.LEC (decrypted)

// Get a helper LFL. id is 901..904.
Span data_helper(int id);          // 9xx.LFL (decrypted)

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
// The engine renders into a 320x200x8bpp main virtual screen. To present it,
// it calls present() with the buffer + palette. The platform layer is
// responsible for the FIT/FILL/CROP scaling and final RGB565 framebuffer
// blit.

enum class ScaleMode : uint8_t {
    Fit  = 0,   // 320x200 -> 128x80 letterboxed (24 px black bars)
    Fill = 1,   // 320x200 -> 128x128 anisotropic stretch
    Crop = 2    // 128x128 native window, pannable via crop_x/y
};

// Submit a frame.
//   virt is 320*200 bytes of palette indices for the main scene.
//   text is 320*200 bytes of palette indices for the kTextVirtScreen
//        overlay; bytes equal to 0xFD (CHARSET_MASK_TRANSPARENCY,
//        scummvm gfx.h:289) mean "no text — fall through to virt".
//        Pass nullptr if no overlay.
//   palette is 256*3 bytes of RGB888 (0..255 range, NOT scumm 6-bit).
//   crop_x/y are used in CROP mode (0..(320-128) and 0..(200-128)).
//
// The text overlay is composited with ink-priority during scaling so
// thin glyph features (1-pixel shadows) survive the 320→128 downsample
// instead of vanishing through nearest-neighbour sampling. The main
// scene uses the ThumbyNES 2x2 packed-RGB565 box blend (md_core.c:601).
void present(const uint8_t *virt, const uint8_t *text,
             const uint8_t *palette,
             ScaleMode mode, int crop_x, int crop_y);

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
struct Input {
    bool dpad_up, dpad_down, dpad_left, dpad_right;
    bool button_a, button_b;
    bool button_lb, button_rb;
    bool button_menu;
    // Edge events (true for one frame on press)
    bool a_pressed, b_pressed, lb_pressed, rb_pressed, menu_pressed;
    bool a_released, b_released, lb_released, rb_released, menu_released;
    // Host-only: real ESC key. On device this is always false; device
    // surfaces cutscene-exit through the LB+RB chord that the engine
    // detects directly.
    bool escape_pressed;

    // Mouse state — host SDL fills these from real mouse input. Device
    // leaves mouse_present=false; engine drives the cursor from
    // dpad+A/B in that case (see engine_tick).
    //   mouse_x/y     — physical screen coords, 0..319 / 0..199
    //   mouse_left/right — held this frame
    //   mouse_*_pressed/_released — one-frame edges (matches scummvm's
    //   _leftBtnPressed & msClicked semantics; see input.cpp:439-456)
    bool mouse_present;
    int  mouse_x, mouse_y;
    bool mouse_left, mouse_right;
    bool mouse_left_pressed,  mouse_right_pressed;
    bool mouse_left_released, mouse_right_released;
};

// Pump platform events; fill Input. Returns false on quit request.
bool poll_input(Input *out);

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
// The engine supplies a callback that produces N int16 mono samples at the
// configured rate. Platform layer drives this from its audio system.
using AudioCallback = void (*)(void *user, int16_t *samples, int n_samples);

// Initialize audio. sample_rate typically 22050. Returns actual rate used.
int  audio_init(int requested_rate, AudioCallback cb, void *user);
void audio_shutdown();

// Single-core device path: refill audio ring with up to one frame's
// worth of new samples. The PWM IRQ drains the ring per-sample on the
// same core. engine_tick calls this once per frame.
// Host SDL is callback-driven by SDL's own audio thread — this is a
// no-op there.
void audio_pump();

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
uint32_t millis();          // monotonic milliseconds since boot
uint32_t micros();          // monotonic microseconds (wraps)
void     sleep_ms(uint32_t ms);

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
// Printf-style debug output. On host: stderr. On device: UART or no-op.
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Fatal error — print and halt. Engine calls this on unrecoverable
// problems (missing resource, unimplemented opcode in critical path).
[[noreturn]] void panic(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

// Boot diagnostic: paint a solid colour splash so we can locate where
// engine_init hangs without a debugger. Device implementation pushes a
// 128×128 RGB565 frame and waits for the LCD DMA to drain. Host
// implementation is a no-op.
void debug_splash(uint16_t rgb565);

}  // namespace tsb::platform
