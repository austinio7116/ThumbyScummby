// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — MI1 charset 1 (small/system font) renderer.
//
// Wraps the baked-in `mi_font.h` blob with simple drawing primitives.
// Used by the sentence strip and all overlay menus (save / verb /
// inventory / dialog) for an authentic LucasArts-style look.

#pragma once

#include <stdint.h>

namespace tsb {
namespace mi_font {

// Pixel width of `str` rendered with the MI font, including 1-px
// inter-glyph spacing.  Stops at the first NUL.
int text_width(const char *str);

// Paint `str` at LCD pixel (x, y).  Each set bit in the glyph bitmap
// is plotted with `fg_rgb565` via tsb::platform::lcd_pixel; clear bits
// are transparent (no write — scene/overlay shows through).  No
// clipping outside [0, 128) — caller keeps coords in range.
void draw(int x, int y, const char *str, uint16_t fg_rgb565);

// Draw substring [start, end) — useful for two-tone sentence painting
// where the verb-prefix is highlighted in a different colour.
void draw_substr(int x, int y, const char *str, int start, int end,
                 uint16_t fg_rgb565);

// Draw `str` clipped to LCD x range [clip_min, clip_max).  Pixels
// outside the band are skipped.  Used to render marquee-scrolling
// rows in pickers where text extends past the row's reserved width.
void draw_clipped(int x, int y, const char *str, uint16_t fg_rgb565,
                  int clip_min, int clip_max);

// Marquee scroll offset.  Returns 0 when `text_w <= lcd_w` (no scroll
// needed); otherwise ping-pongs from 0 to (text_w - lcd_w) with a
// pause at each end.  `frame` is a free-running counter — pass the
// same monotonically-increasing value each render call.  Negate the
// returned offset and add to the text origin x to scroll left.
int marquee_offset(int text_w, int lcd_w, uint32_t frame);

// Per-glyph metadata exposed for callers that want to feed MI glyph
// bitmaps into another rendering pipeline (e.g. OSystem_Thumby's
// SCUMM stamp loop substitutes MI bitmaps when the SPCH FONT toggle
// is on).  Returns true if `c` has a renderable glyph; false for
// out-of-range chars or chars with no bitmap.  Bitmap data is 1bpp
// MSB-first, contiguous (width*height bits, byte-padded).
struct GlyphInfo {
    const uint8_t *bitmap;     // pointer into the static MI bitmap blob;
                               // null when the glyph is non-renderable
                               // (advance-only — e.g. ' ').
    uint8_t  width, height;
    int8_t   offsX, offsY;
    uint8_t  advance;          // pen advance — usually width + 1
};
bool get_glyph(char c, GlyphInfo *out);

}  // namespace mi_font
}  // namespace tsb
