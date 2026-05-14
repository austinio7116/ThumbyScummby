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

// Cursor descriptor passed to present().  Drawing the cursor at LCD
// resolution inside present() avoids the ghosting that an in-staging
// composite would cause (the engine only redraws dirty regions of
// _staging, so an old cursor stamp would persist forever).  Letting
// present() handle it also lets the cursor render at sizes independent
// of the source 320×200 — important in Fit mode where 0.4× downsample
// would otherwise shrink a 16×16 pointer to ~6 LCD pixels.
struct CursorInfo {
    const uint8_t *sprite;       // w*h bytes of palette indices
    int w, h;                    // sprite size in source 320×200 coords
    int hotspot_x, hotspot_y;
    int x, y;                    // hotspot position in 320×200 coords
    uint8_t key_color;           // transparent index
};

// Submit a frame.
//   virt is 320*200 bytes of palette indices for the main scene.
//   text is 320*200 bytes of palette indices for the kTextVirtScreen
//        overlay; bytes equal to 0xFD (CHARSET_MASK_TRANSPARENCY,
//        scummvm gfx.h:289) mean "no text — fall through to virt".
//        Pass nullptr if no overlay.
//   palette is 256*3 bytes of RGB888 (0..255 range, NOT scumm 6-bit).
//   crop_x/y are used in CROP mode (0..(320-128) and 0..(200-128)).
//   cursor optional; nullptr or sprite=nullptr → no cursor drawn.
//
// The text overlay is composited with ink-priority during scaling so
// thin glyph features (1-pixel shadows) survive the 320→128 downsample
// instead of vanishing through nearest-neighbour sampling. The main
// scene uses the ThumbyNES 2x2 packed-RGB565 box blend (md_core.c:601).
//
// THUMBY-PORT — `text_stamps` is an optional list of LCD-native glyph
// stamps (charPtr + dst_x/dst_y + dimensions + cmap snapshot) painted
// directly into the RGB565 framebuffer after the scene blit and before
// the cursor.  Replaces a 128×128 8 bpp overlay buffer (16 KB BSS) — on
// device the overlay didn't fit in the heap budget.
struct TextStamp {
    const uint8_t *charPtr;     // bit-packed glyph rows, MSB-first
    int16_t  dst_x, dst_y;      // top-left in LCD coords
    uint8_t  width, height;     // SOURCE-px dimensions (≤ 32)
    uint8_t  bpp;               // 1, 2, or 4
    uint8_t  flags;             // see kTextStampFlag* below
    uint8_t  cmap[4];           // palette indices (snapshot of charsetColorMap)
    uint8_t  scale_num;         // downsample numerator (e.g. 3 for 75%)
    uint8_t  scale_den;         // downsample denominator (e.g. 4 for 75%);
                                // scale_num == scale_den ⇒ 1:1 fast path
};
// Stamp flag bits.  The scene-only redesign retired the marquee scroll
// path; FullScale is kept as a legacy 1:1 marker (kNum == kDen also
// triggers the same fast path).
constexpr uint8_t kTextStampFlagScroll    = 0x01;   // unused now
constexpr uint8_t kTextStampFlagFullScale = 0x02;

// Sentence strip layout — ALWAYS painted by present() at the bottom of
// the LCD, even with no menu overlay.  Visible during gameplay,
// menus, and dialog mode.  See engine/src/osystem_thumby.cpp for the
// capture path (drawString slot 2).
constexpr int kSentenceLcdRows  = 8;     // matches MI charset 1 height
constexpr int kSceneLcdRows     = 120;   // 128 − sentence strip
constexpr int kSentenceLcdY     = kSceneLcdRows;

// The engine renders a 320×200 framebuffer; we only show source rows
// 0..143 (the scene region).  Rows 144..199 hold the original verb
// panel which we replace with our overlay-based UI, so they're never
// blitted to the LCD.
constexpr int kSceneSrcRows     = 144;

void present(const uint8_t *virt, const uint8_t *text,
             const uint8_t *palette,
             ScaleMode mode, int crop_x, int crop_y,
             const CursorInfo *cursor = nullptr,
             const TextStamp *text_stamps = nullptr,
             int text_stamp_count = 0,
             // Sentence strip — painted with the MI font at LCD rows
             // kSentenceLcdY..127.  `verb_prefix_len` characters are
             // drawn in `prefix_color`, the remainder in `body_color`.
             // Pass nullptr / "" for sentence to leave the strip
             // black this frame.
             const char *sentence = nullptr,
             int verb_prefix_len = 0,
             // When false, present() fills the LCD framebuffer with
             // scene + sentence + cursor but skips the final DMA / SDL
             // push.  Used by overlay menus that want to paint a
             // translucent box on top before sending the composite
             // frame.  After painting the overlay, the caller invokes
             // lcd_present_now() to flush.
             bool send_to_lcd = true,
             // True while the engine is rendering a verb panel into
             // source rows 144..199 (gameplay with player control).
             // When TRUE, present() blits only source rows 0..143 to
             // the LCD; rows 144..199 are hidden because they hold the
             // legacy panel UI we replaced with overlay menus.  When
             // FALSE (cutscenes, title screens, the Melee Island map),
             // present() blits the full 320×200 source so banner text
             // and full-screen scenes display correctly.
             bool panel_active = false,
             // Floating "auto-verb" tooltip rendered next to the
             // cursor sprite (e.g. "bartender" or "Talk to bartender"
             // when the cursor is over an actor).  Empty string or
             // nullptr → nothing painted.
             const char *cursor_tooltip = nullptr);

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

// Force the on-screen log overlay to redraw immediately. Use right
// before a known-fatal call (error(), exit) so the last log line is
// guaranteed visible even if a crash follows. Host: no-op.
void log_flush();

// Log history accessors — power the in-game LOG viewer in save_menu.cpp.
// The platform keeps a ring of the most recent log lines (32 on device,
// fewer or zero on hosts); these read it back out as ASCII.  `idx` runs
// 0 (oldest in the ring) .. log_history_count() - 1 (most recent).
int  log_history_count();
void log_history_get(int idx, char *out, int outsz);

// Fatal error — print and halt. Engine calls this on unrecoverable
// problems (missing resource, unimplemented opcode in critical path).
[[noreturn]] void panic(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

// Boot diagnostic: paint a solid colour splash so we can locate where
// engine_init hangs without a debugger. Device implementation pushes a
// 128×128 RGB565 frame and waits for the LCD DMA to drain. Host
// implementation is a no-op.
void debug_splash(uint16_t rgb565);

// Combined boot checkpoint.  On device: paints `color`.  On host: logs
// `label` plus the current heap-in-use figure (from mallinfo2 on Linux).
// Lets us run the whole init flow once on host and see exactly which
// step would overflow the RP2350's 376 KB heap, instead of bisecting
// with flashed splash builds.
void checkpoint(const char *label, uint16_t color);

// ---------------------------------------------------------------------------
// Save/Load menu — minimal direct-LCD draw primitives
// ---------------------------------------------------------------------------
// The save menu paints onto the LCD framebuffer directly (bypassing the
// engine's 320×200 staging) so we don't need a 64 KB scratch buffer for
// the overlay.  See engine/src/save_menu.cpp.

// Clear the LCD framebuffer to a solid colour.
void lcd_fill(uint16_t rgb565);

// Plot a single pixel at (x, y) with `rgb565`.  No bounds check —
// caller must keep coords in [0, 128).
void lcd_pixel(int x, int y, uint16_t rgb565);

// Push the current framebuffer to the panel and wait for DMA to drain.
void lcd_present_now();

// Non-destructive button polling — read live state without consuming
// edges from the regular input pipeline.  Used by OSystem_Thumby's
// per-frame overlay-trigger detection.
bool is_lb_held();
bool is_rb_held();
bool is_menu_held();
// Non-destructive live state for the buttons we don't already poll
// above.  Used by the Indy4 fight-mode chord handler so it can read
// A/B/d-pad without competing with the engine's main input pipeline.
bool is_a_held();
bool is_b_held();
bool is_dpad_up_held();
bool is_dpad_down_held();

// Darken every LCD pixel inside [x..x+w, y..y+h) by 50% (RGB565 channel
// halve).  Used to render the scene-tinting backdrop of the overlay
// menu box; text painted on top stays opaque.
void lcd_dim_box(int x, int y, int w, int h);

// ---------------------------------------------------------------------------
// Slot mode (ThumbyOne integration)
// ---------------------------------------------------------------------------

// Unmount FatFs (if mounted) and request a return to the ThumbyOne
// lobby via the watchdog-scratch handoff.  Does not return.
//
// Only meaningful in THUMBYONE_SLOT_MODE builds; in standalone
// builds this is a no-op (or unimplemented — the standalone build
// shouldn't expose a "Lobby" menu item).
[[noreturn]] void lobby_handoff();

}  // namespace tsb::platform
