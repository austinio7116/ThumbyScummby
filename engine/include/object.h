// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — object draw and state.
//
// Objects in SCUMM v4/v5 are room-bound interactive elements. Each has:
//   - OBIM small-chunk (image data, multiple states 0..N)
//   - OBCD small-chunk (code header + verb scripts + name)
//
// Objects are drawn with strip-based images similar to room background,
// at fixed positions, only when their `state != 0`.

#pragma once

#include "types.h"

namespace tsb {

struct ObjectData {
    uint16_t obj_id;          // global object number (== ScummVM od.obj_nr)
    uint8_t  x_strip;         // x position in 8-pixel units (multiply by 8)
    uint8_t  y;
    uint8_t  w_strip;         // width in 8-pixel units
    uint8_t  h;
    uint8_t  parentstate;     // expected state of parent for this obj to draw
    uint8_t  parent;          // local-table slot index of parent (0 = none)
    int16_t  walk_x, walk_y;
    uint8_t  actor_dir;

    Span     obim_payload;    // OBIM chunk payload (or empty if no image)
    Span     obcd_payload;    // OBCD chunk payload (for verb scripts)

    uint8_t  state;            // cached from global state table (0 = invisible)
};

// Slot 0 is reserved (matches ScummVM convention — objs[0] is empty;
// drawRoomObjects iterates from _numLocalObjects-1 down to 1; parent==0
// means "no parent"). Local objects live in slots 1..num_objects.
struct ObjectTable {
    int          num_objects;     // count of populated slots (1..num_objects)
    ObjectData   objects[MAX_OBJECTS];
};

void object_init(ObjectTable *t);

// Parse all OBIM/OBCD chunks within a ROOM payload and populate the
// ObjectTable. Returns number of objects loaded.
int  object_load_from_room(Span room_chunk_payload, ObjectTable *t);

// Mark all objects to redraw on next frame.
void object_mark_all_dirty(ObjectTable *t);

// Render each visible object's current state image into the back virtual
// screen (which acts as the background+objects composite — actors will
// draw on top). `buf_w`/`buf_h` give the destination buffer extents
// (== room.width / room.height for the ROOM-wide composite). Defaults
// fall back to pitch / VIRTUAL_SCREEN_H. Mirrors ScummEngine_v5
// drawRoomObjects in object.cpp:620-645, with clipping to the actual
// room rectangle (audit F6).
void object_render_all(const ObjectTable *t,
                       uint8_t *vscreen_back, int pitch,
                       int buf_w = 0, int buf_h = 0);

// Look up object by ID. Returns nullptr if not in table.
ObjectData *object_get_by_id(ObjectTable *t, int obj_id);

// Draw a single object (by obj_id) into the room composite. Mirrors
// ScummVM `processDrawQue`/`drawObject` (object.cpp:1178-1185, 647)
// which paint the queued object's image into the bg buffer the same
// frame the script's o5_drawObject fires. Without this, drawObject
// only updates the object's state; the painted image only appears at
// the next room load.
void object_draw_single(const ObjectTable *t, int obj_id,
                        uint8_t *vscreen_back, int pitch,
                        int buf_w, int buf_h);

// Find which object (if any) is at screen position (x,y). Used by
// findObject opcode and click handling.
int  object_find_at(const ObjectTable *t, int x, int y);

}  // namespace tsb
