// Charset / font rendering for SCUMM v4 small_header.
// Helper files 901-904.LFL contain CHAR resources directly.

#include "charset.h"
#include "platform.h"

#include <string.h>

namespace tsb {

bool charset_load_from_helper(int helper_id, Charset *out) {
    Span helper = platform::data_helper(helper_id);
    if (helper.empty()) return false;

    // Helper file format (v4): starts directly with raw CHAR resource:
    //   uint32 LE  size
    //   uint8      unknown (or part of header)
    //   uint16 LE  colormap_offset
    //   uint8      bpp
    //   uint8      font_height
    //   uint16 LE  num_chars
    //   uint32 LE  char_offsets[num_chars]
    //   ... bit-packed glyph data ...

    if (helper.size < 16) return false;
    out->resource = helper;
    // The first 4 bytes are likely a size header
    const uint8_t *p = helper.data;
    out->bpp           = p[8];
    out->glyph_height  = p[9];
    out->glyph_count   = read_le16(p + 10);
    out->glyph_offsets = p + 12;
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
    if (offs + 4 > cs->resource.size) return;
    const uint8_t *g = cs->resource.data + offs;
    uint8_t w = g[0];
    uint8_t h = g[1];
    int8_t  ox = (int8_t)g[2];
    int8_t  oy = (int8_t)g[3];
    const uint8_t *bits = g + 4;

    int x = dx + ox;
    int y = dy + oy;
    int bit = 0;

    for (int gy = 0; gy < h; gy++) {
        for (int gx = 0; gx < w; gx++) {
            int sx = x + gx, sy = y + gy;
            if (sx < 0 || sx >= VIRTUAL_SCREEN_W ||
                sy < 0 || sy >= VIRTUAL_SCREEN_H) {
                bit += cs->bpp;
                continue;
            }
            uint8_t v = 0;
            for (int b = 0; b < cs->bpp; b++) {
                int bb = bit + b;
                v |= ((bits[bb >> 3] >> (7 - (bb & 7))) & 1) << b;
            }
            bit += cs->bpp;
            if (v != 0) {
                vscreen[sy * pitch + sx] = color_table ? color_table[v] : v;
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
            if (offs > 0 && offs < cs->resource.size) {
                x += cs->resource.data[offs] + 1;
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
