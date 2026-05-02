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

}  // namespace tsb
