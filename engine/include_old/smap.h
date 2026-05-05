// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — SMAP strip-image decoder for SCUMM v4 small_header.
//
// In v4 floppy small-header rooms, the room image is stored in a 'BM'
// (BitMap) chunk:
//
//   uint32 LE   zplane_offs        ; offset to ZP01 data (or 0 if none)
//   uint32 LE   strip_offs[N]      ; N = width / 8 strips, LE offsets
//                                  ; relative to start of BM payload
//   uint8[]     strip_data         ; per-strip compressed pixels
//   uint8[]     zplane_data        ; optional ZP planes after main data
//
// Each strip encodes 8 pixels wide × `height` tall. The first byte of each
// strip is a "compression code" (1, 14-18, 24-28, 34-38, 44-48...) which
// selects the decode method.

#pragma once

#include "types.h"

namespace tsb {

// Decode a 'BM' chunk and rasterize its background pixels into a 320x200
// (or `width x height`) 8bpp paletted buffer.
//
// `bm_payload` is the payload of the BM small-chunk (i.e. bytes after the
// 6-byte header).
//
// Returns false on bad data.
bool smap_decode_bm(Span bm_payload, int width, int height,
                    uint8_t *out_buf, int out_pitch);

// Decode one z-plane (ZP block) into a 1bpp mask buffer. `zp_payload` is
// the entire ZP block: first LE16 = next-plane offset (ignored here),
// then LE16 strip-offset table, then per-strip RLE data
// (gfx.cpp::decompressMaskImg). Strips are 8 px wide; one mask byte per
// row holds 8 horizontal pixels MSB-first.
//
//   width        — pixel width to decode (object width or room width)
//   height       — rows to decode (room/object height; <= MASK rows)
//   mask_buf     — destination mask plane, size `mask_pitch * height`
//   mask_pitch   — row stride of mask_buf in bytes (== row strip count)
//   dst_strip_off— starting strip column to write at (object's x_strip;
//                  0 for room-wide BM-level z-planes)
//   or_mode      — true = OR onto existing bits, false = overwrite.
//                  ScummVM defaults to overwrite (decompressMaskImg);
//                  pass true when applying secondary masks.
//
// Strips whose offset entry is 0 are written as all-zero (per ScummVM
// gfx.cpp:2629-2632) when overwriting, or skipped entirely when OR-ing.
void smap_decode_zplane(Span zp_payload, int width, int height,
                        uint8_t *mask_buf, int mask_pitch,
                        int dst_strip_off, bool or_mode);

}  // namespace tsb
