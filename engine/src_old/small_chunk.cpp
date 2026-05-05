// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
#include "small_chunk.h"

namespace tsb {

bool small_read(Span s, SmallChunk *out) {
    if (s.size < 6) return false;
    uint32_t size = read_le32(s.data);
    uint16_t tag  = read_le16(s.data + 4);
    if (size < 6 || size > s.size) return false;
    out->size    = size;
    out->tag     = tag;
    out->full    = Span{s.data, size};
    out->payload = Span{s.data + 6, size - 6};
    return true;
}

bool small_find(Span parent, uint16_t tag, SmallChunk *out) {
    size_t cur = 0;
    SmallChunk c{};
    while (small_next(parent, &cur, &c)) {
        if (c.tag == tag) {
            *out = c;
            return true;
        }
    }
    return false;
}

bool small_next(Span parent, size_t *cursor, SmallChunk *out) {
    if (*cursor >= parent.size) return false;
    Span rest = parent.sub(*cursor);
    SmallChunk c{};
    if (!small_read(rest, &c)) return false;
    *out = c;
    *cursor += c.size;
    return true;
}

}  // namespace tsb
