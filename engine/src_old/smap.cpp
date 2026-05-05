// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// SMAP strip decoder. Mirrors ScummVM's Gdi::drawStripBasicV /
// drawStripBasicH algorithms (gfx.cpp:4162-...).
//
// Compression code (first byte of each strip) determines decode method:
//   1                  -> RAW256, uncompressed pixel-per-byte
//   14, 15, 16, 17, 18 -> ZIGZAG vertical, N-bit palette indices
//   24, 25, 26, 27, 28 -> ZIGZAG horizontal
//   34..38             -> vertical, transparent variant
//   44..48             -> horizontal, transparent variant
//
// Bit stream is MSB-first within bytes.

#include "smap.h"
#include "platform.h"

#include <string.h>

namespace tsb {

// SCUMM bit reader: LSB-first within bytes, refill when buffer < 8.
// Mirrors gfx.cpp:4101-4107 exactly.
//
//   #define READ_BIT  (cl--, bit = bits & 1, bits >>= 1, bit)
//   #define FILL_BITS  if (cl <= 8) { bits |= (*src++) << cl; cl += 8; }

#define BR_DECL                                  \
    uint32_t bits = 0; int cl = 0; uint8_t bit = 0
#define BR_INIT(src8)                            \
    do { bits = (src8); cl = 8; } while (0)
#define BR_FILL                                  \
    do { if (cl <= 8 && src < src_end) { bits |= ((uint32_t)(*src++)) << cl; cl += 8; } } while (0)
#define BR_READ_BIT                              \
    (cl--, bit = (uint8_t)(bits & 1), bits >>= 1, bit)
#define BR_READ_N(nbits, out)                    \
    do { (out) = (uint8_t)(bits & ((1u << (nbits)) - 1)); \
         bits >>= (nbits); cl -= (nbits); } while (0)

// Decode a single 8-pixel-wide vertical-zigzag strip.
// `shift` = bits per palette index (e.g. 4 for code 14, 5 for code 15).
// `transp_check` = if true, transparent_color pixels are skipped.
static void decode_strip_v(const uint8_t *src, const uint8_t *src_end,
                           int height, int shift, bool transp_check,
                           uint8_t transp_color,
                           uint8_t *dst, int pitch) {
    if (src + 2 > src_end) return;
    uint8_t color = *src++;
    BR_DECL;
    BR_INIT(*src++);
    int8_t inc = -1;

    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < height; y++) {
            BR_FILL;
            if (!transp_check || color != transp_color) {
                dst[y * pitch + x] = color;
            }
            if (!BR_READ_BIT) {
                // same color
            } else {
                if (!BR_READ_BIT) {
                    BR_FILL;
                    BR_READ_N(shift, color);
                    inc = -1;
                } else {
                    if (!BR_READ_BIT) {
                        color = (uint8_t)(color + inc);
                    } else {
                        inc   = (int8_t)-inc;
                        color = (uint8_t)(color + inc);
                    }
                }
            }
        }
    }
}

static void decode_strip_h(const uint8_t *src, const uint8_t *src_end,
                           int height, int shift, bool transp_check,
                           uint8_t transp_color,
                           uint8_t *dst, int pitch) {
    if (src + 2 > src_end) return;
    uint8_t color = *src++;
    BR_DECL;
    BR_INIT(*src++);
    int8_t inc = -1;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < 8; x++) {
            BR_FILL;
            if (!transp_check || color != transp_color) {
                dst[y * pitch + x] = color;
            }
            if (!BR_READ_BIT) {
                // same
            } else {
                if (!BR_READ_BIT) {
                    BR_FILL;
                    BR_READ_N(shift, color);
                    inc = -1;
                } else {
                    if (!BR_READ_BIT) {
                        color = (uint8_t)(color + inc);
                    } else {
                        inc   = (int8_t)-inc;
                        color = (uint8_t)(color + inc);
                    }
                }
            }
        }
    }
}

static void decode_strip_raw(const uint8_t *src, const uint8_t *src_end,
                             int height, uint8_t *dst, int pitch) {
    int total = 8 * height;
    int avail = (int)(src_end - src);
    if (total > avail) total = avail;
    // Pixels stored in scan order: row-major, 8 pixels per row of strip
    for (int i = 0; i < total; i++) {
        int x = i % 8;
        int y = i / 8;
        dst[y * pitch + x] = src[i];
    }
}

bool smap_decode_bm(Span bm_payload, int width, int height,
                    uint8_t *out_buf, int out_pitch) {
    if (width <= 0 || height <= 0) return false;
    if (width % 8 != 0) {
        platform::log("smap: width %d not divisible by 8\n", width);
        return false;
    }
    int num_strips = width / 8;
    if (bm_payload.size < (size_t)4 + (size_t)num_strips * 4) {
        platform::log("smap: BM payload too small (%zu B for %d strips)\n",
                      bm_payload.size, num_strips);
        return false;
    }

    // Read strip offset table (offsets are relative to start of BM payload).
    // The very first uint32 is the offset to ZP01 data (or 0).
    const uint8_t *base   = bm_payload.data;
    const uint8_t *base_end = base + bm_payload.size;
    uint32_t zplane_offs = read_le32(base);
    (void)zplane_offs;

    (void)zplane_offs;

    // Strip offsets follow. Offset[i] points to start of strip i's data.
    // Each strip is 8 pixels wide. We output into a buffer of pitch
    // out_pitch — strips whose left edge >= out_pitch are off-screen and
    // must NOT be written or they wrap into the next row, producing the
    // "noise band" we used to see at the top of the Lucasfilm screen.
    // (ScummVM keeps a separate room-width backbuffer and blits a
    // viewport-sized window from it; we go direct to the screen so we
    // clip here instead.)
    for (int s = 0; s < num_strips; s++) {
        if (s * 8 >= out_pitch) break;
        uint32_t off = read_le32(base + 4 + s * 4);
        if (off == 0 || off >= bm_payload.size) {
            // Empty / out-of-bounds strip — leave blank
            continue;
        }
        const uint8_t *strip_data = base + off;
        // Determine decode method from the first byte
        uint8_t code = strip_data[0];
        const uint8_t *src = strip_data + 1;

        uint8_t *dst = out_buf + s * 8;  // strip column start

        if (code == 1) {
            decode_strip_raw(src, base_end, height, dst, out_pitch);
        } else if (code >= 14 && code <= 18) {
            int shift = code - 10;
            decode_strip_v(src, base_end, height, shift, false, 0, dst, out_pitch);
        } else if (code >= 24 && code <= 28) {
            int shift = code - 20;
            decode_strip_h(src, base_end, height, shift, false, 0, dst, out_pitch);
        } else if (code >= 34 && code <= 38) {
            int shift = code - 30;
            decode_strip_v(src, base_end, height, shift, true, 0, dst, out_pitch);
        } else if (code >= 44 && code <= 48) {
            int shift = code - 40;
            decode_strip_h(src, base_end, height, shift, true, 0, dst, out_pitch);
        } else {
            // Unknown code — fill with diagnostic color so it stands out
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < 8; x++) {
                    dst[y * out_pitch + x] = (uint8_t)(0xE0 + (code & 0xF));
                }
            }
            platform::log("smap: unknown strip code %u in strip %d\n", code, s);
        }
    }
    return true;
}

// Mirrors ScummVM Gdi::decompressMaskImg (gfx.cpp:3115) — RLE column.
// Source byte b: high bit set => run-of-c (b & 0x7F times), else literal
// (b copies, each a fresh byte). dst advances by mask_pitch per row.
static void decompress_mask_column(const uint8_t *src, const uint8_t *src_end,
                                   uint8_t *dst, int mask_pitch, int height,
                                   bool or_mode) {
    while (height > 0 && src < src_end) {
        uint8_t b = *src++;
        if (b & 0x80) {
            int n = b & 0x7F;
            if (src >= src_end) return;
            uint8_t c = *src++;
            while (n-- > 0 && height > 0) {
                if (or_mode) *dst |= c;
                else         *dst  = c;
                dst += mask_pitch;
                height--;
            }
        } else {
            int n = b;
            while (n-- > 0 && height > 0) {
                if (src >= src_end) return;
                uint8_t v = *src++;
                if (or_mode) *dst |= v;
                else         *dst  = v;
                dst += mask_pitch;
                height--;
            }
        }
    }
}

void smap_decode_zplane(Span zp_payload, int width, int height,
                        uint8_t *mask_buf, int mask_pitch,
                        int dst_strip_off, bool or_mode) {
    if (zp_payload.empty() || !mask_buf) return;
    if (width <= 0 || height <= 0) return;
    if (width % 8 != 0) return;
    int num_strips = width / 8;

    const uint8_t *base     = zp_payload.data;
    const uint8_t *base_end = base + zp_payload.size;
    // The ZP per-strip offset table starts at +2 (after the next-plane
    // LE16). gfx.cpp:2611-2612 GF_SMALL_HEADER:
    //   offs = LE16 at zplane[i] + stripnr*2 + 2.
    // Need 2 (next-plane) + 2 * num_strips bytes header minimum.
    if (zp_payload.size < (size_t)2 + (size_t)num_strips * 2) return;

    for (int s = 0; s < num_strips; s++) {
        int dst_strip = dst_strip_off + s;
        if (dst_strip < 0 || dst_strip >= mask_pitch) continue;
        uint16_t off = read_le16(base + 2 + s * 2);
        uint8_t *col = mask_buf + dst_strip;
        if (off == 0) {
            // gfx.cpp:2629-2632: zero the column on overwrite, leave
            // alone on OR (per !dbAllowMaskOr branch).
            if (!or_mode) {
                for (int y = 0; y < height; y++) {
                    col[y * mask_pitch] = 0;
                }
            }
            continue;
        }
        if ((size_t)off >= zp_payload.size) continue;
        decompress_mask_column(base + off, base_end, col, mask_pitch,
                               height, or_mode);
    }
}

}  // namespace tsb
