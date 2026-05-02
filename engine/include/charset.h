// ThumbyScummby — bitmap charset / font rendering.
// Loaded from 9xx.LFL or from in-room CHAR chunks.

#pragma once

#include "types.h"

namespace tsb {

struct Charset {
    Span     resource;          // Span pointing into XIP-resident bytes (decrypted)
    int      glyph_height;
    int      glyph_count;
    int      bpp;               // bits per pixel: 1, 2, 4, 8
    const uint8_t *glyph_offsets; // table: uint32_t per glyph
    const uint8_t *colormap;      // 16 bytes color remap table for 4bpp glyphs
};

bool charset_load_from_helper(int helper_id, Charset *out);

// Render a single glyph at (dx, dy) on the 8bpp virtual screen.
// `color_table` (16 bytes) maps glyph nibble values to palette indices.
void charset_draw_char(const Charset *cs, char c, int dx, int dy,
                       const uint8_t *color_table,
                       uint8_t *vscreen, int pitch);

// Render a 0-terminated string at (dx, dy). Returns the next x pixel.
int  charset_draw_string(const Charset *cs, const char *s, int dx, int dy,
                         const uint8_t *color_table,
                         uint8_t *vscreen, int pitch);

}  // namespace tsb
