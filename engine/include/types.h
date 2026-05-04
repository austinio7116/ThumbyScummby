// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — basic types, FOURCC helpers, byte-order utilities.
// No standard library beyond <stdint.h>/<stddef.h>.

#pragma once

#include <stdint.h>
#include <stddef.h>

// FourCC packing — chunks on disk are big-endian 4-char tags.
#define FOURCC(a, b, c, d) \
    ((uint32_t)(uint8_t)(a) << 24 | (uint32_t)(uint8_t)(b) << 16 | \
     (uint32_t)(uint8_t)(c) <<  8 | (uint32_t)(uint8_t)(d))

namespace tsb {

// Big/little-endian readers from raw memory pointers. No alignment assumed.
inline uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
inline uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}
inline uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[1] << 8) | (uint16_t)p[0]);
}
inline uint32_t read_le32(const uint8_t *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] <<  8) |  (uint32_t)p[0];
}
inline int16_t  read_le16s(const uint8_t *p) { return (int16_t)read_le16(p); }
inline int32_t  read_le32s(const uint8_t *p) { return (int32_t)read_le32(p); }

// A view onto bytes. Used everywhere — usually points into XIP-mapped flash
// (device) or mmap'd file (host). Never owns memory.
struct Span {
    const uint8_t *data;
    size_t         size;

    bool empty() const { return size == 0 || data == nullptr; }
    Span sub(size_t off, size_t n) const {
        if (off > size) return Span{nullptr, 0};
        size_t avail = size - off;
        return Span{data + off, n < avail ? n : avail};
    }
    Span sub(size_t off) const { return sub(off, size); }
};

// Constants
constexpr int VIRTUAL_SCREEN_W = 320;       // visible viewport (matches ScummVM _screenWidth for v4)
constexpr int VIRTUAL_SCREEN_H = 200;       // viewport height
constexpr int ROOM_BUFFER_W    = 1024;      // room-wide backbuffer; max v4 room is 800
constexpr int NUM_STRIPS       = VIRTUAL_SCREEN_W / 8;   // 40 strips visible at once
constexpr int DISPLAY_W        = 128;
constexpr int DISPLAY_H        = 128;
constexpr int MAX_ACTORS       = 16;
constexpr int MAX_SCRIPT_SLOTS = 24;
constexpr int NUM_GLOBALS      = 800;
constexpr int NUM_BIT_VARS     = 4096;     // 512 bytes
constexpr int NUM_LOCALS       = 26;
constexpr int MAX_OBJECTS      = 200;
constexpr int MAX_VERBS        = 32;
constexpr int MAX_BOXES        = 64;
// SCUMM v4 small_header rooms encode at most four ZP planes. ScummVM
// asserts numZBuffer <= 8 (gfx.cpp:1067) but the v4 walker caps the
// chain at 4 (gfx.cpp:1051). Plane index 0 is "no clip" — actor's
// _zbuf 1..N_ZPLANES selects masks[0..N_ZPLANES-1].
constexpr int MAX_ZPLANES      = 4;
// Z-mask buffer is room-wide so a single decode at room load suffices.
// 1bpp packed: ROOM_BUFFER_W / 8 = 128 bytes per row, 200 rows.
constexpr int MASK_BUF_PITCH   = ROOM_BUFFER_W / 8;
constexpr int MASK_BUF_SIZE    = MASK_BUF_PITCH * VIRTUAL_SCREEN_H;

}  // namespace tsb
