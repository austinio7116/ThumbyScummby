// ThumbyScummby — engine entry points.
//
// The platform layer provides startup data and pumps events; the engine
// runs the game.

#pragma once

#include "types.h"
#include "platform.h"

namespace tsb {

// Initialize the engine. Game data must already be installed via
// platform::data_*(). Returns true on success. After this returns, call
// engine_tick() once per frame.
bool engine_init();

// Run one frame: process input, advance scripts, render. Returns false to
// request a quit.
bool engine_tick();

// Optional: cleanly shut down. Free's nothing on device (no malloc).
void engine_shutdown();

// Synchronously change rooms (used by op_loadRoom so entry/exit scripts can
// run nested in the same dispatch tick, matching ScummVM startScene).
// Returns true if the new room was loaded (or new_room == 0).
bool engine_change_room(int new_room);

// Accessors used by op_loadRoom to fetch the OLD-room exit code (EXCD)
// and the NEW-room entry code (ENCD). Returns an empty Span if the room
// has no such chunk.
Span engine_room_excd_payload();   // for the OLD/current room
Span engine_room_encd_payload();   // for the new (now-current) room
// Offset of EXCD/ENCD payload within the room resource buffer — matches
// ScummVM's _EXCD_offs / _ENCD_offs and is used as trace_pc_offset for
// the running slot.
uint32_t engine_room_excd_offset();
uint32_t engine_room_encd_offset();

// The engine's tracked current-room ID (mirrors ScummVM `_currentRoom`).
// May diverge briefly from VAR(VAR_ROOM) — startScene's same-room shortcut
// (script_v5.cpp:1849) compares against `_currentRoom`.
int engine_current_room_id();

// Fetch the bytecode payload for a local script of the current room (id
// in the range 200..259 for v4). Returns empty span if not present.
// `out_offset` receives the byte offset of the payload within the room
// resource base, suitable for `vm_start_room_script`'s pc_offset.
Span engine_local_script(int script_id, uint32_t *out_offset);

// Global object state/owner tables — mirrors ScummVM's _objectStateTable /
// _objectOwnerTable indexed by global obj_nr. Zero-initialised at boot.
// Object visibility in the renderer is gated by these.
constexpr int NUM_GLOBAL_OBJECTS = 1024;       // upper bound for v4 MI1
uint8_t engine_get_object_state(int obj_id);
void    engine_put_object_state(int obj_id, uint8_t state);
uint8_t engine_get_object_owner(int obj_id);
void    engine_put_object_owner(int obj_id, uint8_t owner);

// Camera control — mirrors ScummEngine setCameraAt / panCameraTo /
// setCameraFollows. Called from the matching opcode handlers.
void engine_camera_set_at(int pos_x);
void engine_camera_pan_to(int x);
void engine_camera_set_follows(int actor_num, bool force);

}  // namespace tsb
