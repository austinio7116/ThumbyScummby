// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — MI1 charset 1 font renderer.

#include "mi_font_render.h"
#include "mi_font.h"
#include "platform.h"

namespace tsb {
namespace mi_font {

namespace {

// Resolve a char to its glyph index (0..kCharCount-1) or -1 if out of range.
inline int idx(char c) {
	const int u = static_cast<unsigned char>(c);
	if (u < kFirstChar || u > kLastChar) return -1;
	return u - kFirstChar;
}

inline int advance(const Glyph &g) {
	// 1 px inter-glyph spacing matches the SCUMM panel layout.
	return g.width > 0 ? g.width + 1 : 4;  // unrenderable chars get a 4 px gap
}

}  // anonymous

int text_width(const char *str) {
	if (!str) return 0;
	int w = 0;
	for (; *str; ++str) {
		const int i = idx(*str);
		if (i < 0) { w += 4; continue; }
		w += advance(kGlyphs[i]);
	}
	if (w > 0) w -= 1;   // last char doesn't carry trailing spacing
	return w;
}

void draw_substr(int x, int y, const char *str, int start, int end,
                 uint16_t fg) {
	if (!str) return;
	int pen = x;
	int char_pos = 0;
	for (; *str; ++str, ++char_pos) {
		const int i = idx(*str);
		if (i < 0) { pen += 4; continue; }
		const Glyph &g = kGlyphs[i];
		const bool paint = (char_pos >= start && char_pos < end) && g.width > 0;
		if (paint && g.bitmap_offset != 0xFFFF) {
			// Decode contiguous 1bpp MSB-first bitstream.
			const uint8_t *bm = &kBitmaps[g.bitmap_offset];
			int bit = 0;
			for (int gy = 0; gy < g.height; ++gy) {
				for (int gx = 0; gx < g.width; ++gx) {
					if (bm[bit >> 3] & (0x80 >> (bit & 7))) {
						tsb::platform::lcd_pixel(pen + gx + g.offsX,
						                         y + gy + g.offsY, fg);
					}
					++bit;
				}
			}
		}
		pen += advance(g);
	}
}

void draw(int x, int y, const char *str, uint16_t fg) {
	if (!str) return;
	int len = 0;
	for (const char *p = str; *p; ++p) ++len;
	draw_substr(x, y, str, 0, len, fg);
}

}  // namespace mi_font
}  // namespace tsb
