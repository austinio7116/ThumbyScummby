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

}  // namespace mi_font
}  // namespace tsb
