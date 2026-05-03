// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// Charset / font rendering for SCUMM v4 small_header.
// Helper files 901-904.LFL contain CHAR resources directly.

#include "charset.h"
#include "platform.h"

#include <string.h>

namespace tsb {

bool charset_load_from_helper(int helper_id, Charset *out) {
    Span helper = platform::data_helper(helper_id);
    if (helper.empty()) return false;

    // Helper file layout (9xx.LFL):
    //   +0 ..  +3  uint32 LE   file size (consumed by ScummVM's resource
    //                          loader before populating its buffer)
    //   +4 .. +20  17-byte CHAR pre-header (unused fields + colormap)
    //  +21         bpp
    //  +22         glyph_height
    //  +23..+24    uint16 LE   num_chars
    //  +25..       uint32 LE * num_chars   per-glyph offsets
    //  ...         bit-packed glyph data
    //
    // ScummVM's CharsetRendererCommon::setCurID does `_fontPtr += 17` after
    // pointing past the size word, so `_fontPtr` lands at file + 21.

    if (helper.size < 25) return false;
    out->resource      = helper;
    out->fontptr       = helper.data + 21;
    out->bpp           = out->fontptr[0];
    out->glyph_height  = out->fontptr[1];
    out->glyph_count   = read_le16(out->fontptr + 2);
    out->glyph_offsets = out->fontptr + 4;
    out->colormap      = nullptr;
    if (out->bpp != 1 && out->bpp != 2 && out->bpp != 4 && out->bpp != 8) {
        platform::log("charset: helper %d bpp=%d invalid\n",
                      helper_id, out->bpp);
        return false;
    }
    platform::log("charset: helper %d -> %d glyphs, %dpx tall, %d bpp\n",
                  helper_id, out->glyph_count, out->glyph_height, out->bpp);
    return true;
}

void charset_draw_char(const Charset *cs, char c, int dx, int dy,
                       const uint8_t *color_table,
                       uint8_t *vscreen, int pitch) {
    int idx = (uint8_t)c;
    if (idx >= cs->glyph_count) return;

    uint32_t offs = read_le32(cs->glyph_offsets + idx * 4);
    if (offs == 0) return;
    // Glyph offsets are relative to _fontPtr (= helper.data + 21).
    size_t fontptr_base = (size_t)(cs->fontptr - cs->resource.data);
    if (fontptr_base + offs + 4 > cs->resource.size) return;
    const uint8_t *g = cs->fontptr + offs;
    uint8_t w = g[0];
    uint8_t h = g[1];
    int8_t  ox = (int8_t)g[2];
    int8_t  oy = (int8_t)g[3];
    const uint8_t *bits = g + 4;

    int x = dx + ox;
    int y = dy + oy;

    // Mirrors CharsetRendererClassic::drawBitsN
    // (scummvm-upstream/engines/scumm/charset.cpp:1391-1434):
    //   bits = *src++; numbits = 8;
    //   for each pixel: color = (bits >> (8 - bpp)) & 0xFF;
    //                   bits <<= bpp; numbits -= bpp;
    //                   if numbits==0 refill from *src++
    // The pixel value is the TOP `bpp` bits of `bits` (MSB-first
    // packing); a previous reading of this routine accumulated LSB-
    // first so 2bpp glyphs came out bit-reversed (pixel "10" -> 1
    // instead of 2), which made readable English render as gibberish.
    int bpp = cs->bpp;
    if (bpp <= 0 || bpp > 8) return;
    uint8_t bits_buf = *bits++;
    int numbits = 8;
    for (int gy = 0; gy < h; gy++) {
        for (int gx = 0; gx < w; gx++) {
            uint8_t color = (uint8_t)((bits_buf >> (8 - bpp)) & 0xFF);
            int sx = x + gx, sy = y + gy;
            if (color != 0 &&
                sx >= 0 && sx < VIRTUAL_SCREEN_W &&
                sy >= 0 && sy < VIRTUAL_SCREEN_H) {
                vscreen[sy * pitch + sx] =
                    color_table ? color_table[color] : color;
            }
            bits_buf = (uint8_t)(bits_buf << bpp);
            numbits -= bpp;
            if (numbits == 0) {
                bits_buf = *bits++;
                numbits = 8;
            }
        }
    }
}

int charset_draw_string(const Charset *cs, const char *s, int dx, int dy,
                        const uint8_t *color_table,
                        uint8_t *vscreen, int pitch) {
    int x = dx;
    while (*s) {
        char c = *s++;
        if (c == '\n') { x = dx; dy += cs->glyph_height + 1; continue; }
        charset_draw_char(cs, c, x, dy, color_table, vscreen, pitch);
        // Glyph width — read from glyph header
        int idx = (uint8_t)c;
        if (idx < cs->glyph_count) {
            uint32_t offs = read_le32(cs->glyph_offsets + idx * 4);
            size_t fontptr_base = (size_t)(cs->fontptr - cs->resource.data);
            if (offs > 0 && fontptr_base + offs < cs->resource.size) {
                x += cs->fontptr[offs] + 1;
            } else {
                x += 4;
            }
        } else {
            x += 4;
        }
    }
    return x;
}

}  // namespace tsb
