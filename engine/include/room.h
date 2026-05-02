// ThumbyScummby — room loader. Locates a room within DISK0n.LEC, parses
// its sub-chunks (RMHD, palette, RMIM/SMAP, OBIM/OBCD, scripts, BOXD), and
// renders the background into a target 320x200 8bpp buffer.

#pragma once

#include "types.h"
#include "master_index.h"

namespace tsb {

// Information about a parsed room. Pointers into XIP-resident data — never
// owns memory.
struct Room {
    int      id;
    int      width;
    int      height;
    int      num_objects;

    // The top-of-room ROOM chunk and its commonly-needed sub-chunk payloads.
    Span     room_chunk;       // entire ROOM small-chunk payload
    Span     rmhd_payload;
    Span     rmim_payload;     // contains nested SMAP + ZP01..ZP04
    Span     bm_smap_payload;  // SMAP body inside RMIM
    Span     palette_payload;  // PA / SL / CL — whichever the room has
    Span     boxd_payload;     // BOXD (BX) — walkbox table
    Span     boxm_payload;     // BOXM ('BM' as a sibling, not the bitmap)

    uint8_t  transparent_color;
};

// Load room with ID `room_id`. Returns false if not found / not on a
// loaded disk.
bool room_load(int room_id, const MasterIndex &master, Room *out);

// Render the room's background image into a 320x200 8bpp buffer. Returns
// false on decode error. `out_buf` must be `>= 320*height` bytes (we write
// up to room.width × room.height pixels; for non-320-wide rooms we don't
// pad).
bool room_render_background(const Room &room, uint8_t *out_buf, int out_pitch);

// Convert the room's PALS payload into 256 RGB888 triplets in `out_palette`
// (768 bytes). Handles 6-bit→8-bit upscaling.
bool room_load_palette(const Room &room, uint8_t *out_palette);

}  // namespace tsb
