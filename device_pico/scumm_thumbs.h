// Baked-in 64x64 4-bit indexed thumbnails for the SCUMM picker.
// One entry per supported game.  See pack_scumm_thumbs.py for the
// generator + bit packing (two pixels per byte, even pixel in the
// high nibble, palette is 16 entries of RGB565).
//
// The picker matches by `subdir` against the GameDescriptor it's
// rendering, falling back to a placeholder if the descriptor has
// no matching thumbnail (e.g. user added a new game to the table
// but hasn't dropped a PNG into device_pico/thumbs/ yet).

#pragma once

#ifdef __cplusplus
#include <cstdint>
extern "C" {
#else
#include <stdint.h>
#endif

#define SCUMM_THUMB_W 64
#define SCUMM_THUMB_H 64

typedef struct {
    const char     *subdir;     // matches GameDescriptor::subdir
    const uint16_t *palette;    // 16 RGB565 entries
    const uint8_t  *pixels;     // 64 * 64 / 2 bytes (two pixels per byte)
} scumm_thumb_t;

extern const scumm_thumb_t scumm_thumbs[];
extern const int           scumm_thumbs_count;

#ifdef __cplusplus
}  // extern "C"
#endif
