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
    uint16_t obj_id;
    uint8_t  x_strip;     // x position in 8-pixel units (multiply by 8)
    uint8_t  y;
    uint8_t  w_strip;     // width in 8-pixel units
    uint8_t  h;
    uint8_t  flags;
    uint8_t  parent;
    int16_t  walk_x, walk_y;
    uint8_t  actor_dir;

    Span     obim_payload;     // OBIM chunk payload (or empty if no image)
    Span     obcd_payload;     // OBCD chunk payload (for verb scripts)

    uint8_t  state;            // current state (0 = invisible/erased)
};

struct ObjectTable {
    int          num_objects;
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
// draw on top).
void object_render_all(const ObjectTable *t,
                       uint8_t *vscreen_back, int pitch);

// Look up object by ID. Returns nullptr if not in table.
ObjectData *object_get_by_id(ObjectTable *t, int obj_id);

// Find which object (if any) is at screen position (x,y). Used by
// findObject opcode and click handling.
int  object_find_at(const ObjectTable *t, int x, int y);

}  // namespace tsb
