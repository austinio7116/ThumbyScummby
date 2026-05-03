// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
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
    // ScummVM's _fontPtr — points 17 bytes past the resource body start.
    // For helper LFL files (4-byte LE size + body) that lands at file+21.
    // bpp / height / numChars are at fontPtr[0..3]; per-glyph LE32 offset
    // table at fontPtr+4; glyph data at fontPtr + table[chr].
    const uint8_t *fontptr;
    const uint8_t *glyph_offsets; // table: uint32_t per glyph (= fontptr+4)
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
