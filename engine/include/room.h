// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — room loader. Locates a room within DISK0n.LEC, parses
// its sub-chunks (RMHD, palette, RMIM/SMAP, OBIM/OBCD, scripts, BOXD), and
// renders the background into a target 320x200 8bpp buffer.

#pragma once

#include "types.h"
#include "master_index.h"

namespace tsb {

// Per-room palette cycle entry — mirrors ScummVM ColorCycle (gfx.h:279).
// In v4 small_header, the CYCL chunk ('CC' tag) holds exactly 16 entries
// of (BE16 delay, u8 start, u8 end). counter is initialised at load time
// to `start` if delay != 0 && delay != 0x0AAA (palette.cpp:618).
struct ColorCycle {
    uint16_t delay;
    uint16_t counter;
    uint8_t  start;
    uint8_t  end;
};

// Information about a parsed room. Pointers into XIP-resident data — never
// owns memory.
struct Room {
    int      id;
    int      width;
    int      height;
    int      num_objects;
    ColorCycle color_cycle[16];

    // The top-of-room ROOM chunk and its commonly-needed sub-chunk payloads.
    Span     room_chunk;       // entire ROOM small-chunk payload
    Span     rmhd_payload;
    Span     rmim_payload;     // contains nested SMAP + ZP01..ZP04
    Span     bm_smap_payload;  // SMAP body inside RMIM
    Span     palette_payload;  // PA / SL / CL — whichever the room has
    Span     boxd_payload;     // BOXD (BX) — walkbox table
    Span     boxm_payload;     // BOXM ('BM' as a sibling, not the bitmap)

    // ENCD / EXCD — room-entry / room-exit local scripts (run by ScummVM's
    // runEntryScript / runExitScript when the room changes). The payloads
    // are bytecode chunks with NO 6-byte small-chunk header; we feed them
    // straight to vm_start_room_script.
    Span     encd_payload;
    Span     excd_payload;
    // Offset of each chunk's body within the room resource (matches
    // ScummVM's _ENCD_offs / _EXCD_offs). Used for trace alignment.
    uint32_t encd_offset;
    uint32_t excd_offset;
    // Base of the room resource as ScummVM holds it — i.e. the start of
    // the ROOM full chunk (with its 6-byte header). We compute offsets
    // relative to this so they line up with ScummVM's _scriptOrgPointer.
    Span     room_resource;

    // Local scripts (LSCR chunks). Index by (id - 200). Stores the
    // bytecode payload (after the 1-byte LSCR ID prefix) AND the offset
    // of that bytecode within the room resource (matches ScummVM's
    // _localScriptOffsets). Empty span = script not present.
    static constexpr int MAX_LOCAL_SCRIPTS = 60;
    Span     lscr_payload[MAX_LOCAL_SCRIPTS];
    uint32_t lscr_offset[MAX_LOCAL_SCRIPTS];

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

// Advance every active cycle by one tick. For each cycle whose counter is
// non-zero, increments the counter (wrapping start..end), and rotates the
// RGB triplets at indices [start..end] left by one. Mirrors the v4 path
// of ScummEngine::cyclePalette (palette.cpp:741-768) using direct palette
// rotation rather than ScummVM's _shadowPalette indirection.
void palette_cycle_tick(ColorCycle cycles[16], uint8_t *palette);

}  // namespace tsb
