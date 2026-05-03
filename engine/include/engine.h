// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
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

// True when running under v4 (GF_SMALL_HEADER) opcode dispatch — set by
// vm_opcodes_v4_init(). Used by opcodes that take different operand
// shapes between v4 and v5+ (notably o5_drawObject — see script_v5.cpp:
// 1041-1043).
bool engine_is_v4();

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

// Mirror ScummEngine::_classData[obj] (object.cpp:225-294). Up to 32
// class bits per object, with the SMALL_HEADER class-id remap applied
// (kObjectClassUntouchable→24, Player→23, XFlip→19, YFlip→18). Negative
// `cls` carries a polarity flag in scripts (`cls & 0x80` → set=true).
bool     engine_get_class(int obj_id, int cls);
void     engine_put_class(int obj_id, int cls, bool set);
void     engine_clear_class_data(int obj_id);

// Inventory ownership constants. ScummVM uses VAR(VAR_EGO) as the
// "carried by player" owner; objects in a room have owner == 0xFF.
constexpr uint8_t OWNER_ROOM = 0xFF;

// Inventory pool — global inventory belongs to actors; ScummVM picks
// up an object by setting its owner to VAR_EGO and assigning its
// inventory slot via _inventory[]. See object.cpp:addObjectToInventory.
//
// `engine_inventory_count(actor)` mirrors getInventoryCount.
// `engine_find_inventory(actor, n)` mirrors findInventory (1-based n).
int      engine_inventory_count(int actor_num);
int      engine_find_inventory(int actor_num, int n);
void     engine_add_object_to_inventory(int obj_id, int actor_num);
void     engine_remove_object_from_inventory(int obj_id);

// Run all VAR_TIMER / VAR_TIMER_TOTAL / VAR_TMR_1/2/3 updates, mirrors
// scumm.cpp:2989-3008 scummLoop_updateScummVars + decreaseScriptDelay.
// Called at top of every engine_tick before scripts run.
void     engine_update_scumm_vars(int delta_ticks);

// Look up an object's current position. For v4/v5 ScummEngine objects
// `walk_x` / `walk_y` (the on-disk OBCD `walk_x` / `walk_y` fields)
// are the click-here coords; this matches the v3+ branch of
// ScummEngine::getObjectXYPos. Returns false if obj isn't loaded in
// the current room.
bool     engine_object_walk_pos(int obj_id, int *out_x, int *out_y, int *out_dir);

// Click-on-object lookup mirroring ScummEngine::findObject (object.cpp:557).
// Honours the SMALL_HEADER kObjectClassUntouchable remap (class 24).
int      engine_find_object_at(int x, int y);

// Distance between two world points (object/actor positions). Mirrors
// ScummEngine::getDist (object.cpp:516) which is `MAX(|dy|,|dx|)`.
int      engine_world_dist(int x1, int y1, int x2, int y2);

// Walk an actor toward an object's `walk_x/walk_y` and (per ScummVM)
// queue a face-toward direction once the actor arrives. Mirrors
// o5_walkActorToObject behaviour (script_v5.cpp:2120-2158).
void     engine_walk_actor_to_object(int actor_num, int obj_id);
// Snap an actor onto an object's walk position. Mirrors
// o5_putActorAtObject (script_v5.cpp:2133-2158).
void     engine_put_actor_at_object(int actor_num, int obj_id);

// Direct pixel-aligned filled box on the main viewport surface.
// Mirrors ScummEngine::drawBox (gfx.cpp). Coordinates are room-space
// (bg buffer) — caller already has a wide-room buffer. We render into
// vscreen_room (the room-wide composite); next viewport blit picks it
// up. -1 sentinel for `color` matches ScummVM's "background colour".
void     engine_draw_box(int x1, int y1, int x2, int y2, int color);

// Object-name store. Mirrors ScummEngine's rtObjectName resource pool
// (loaded via op_setObjectName / OBNA chunks). Returns nullptr when no
// name is set. Used by string convertMessageToString \xFF\x06 and by
// the verb bar.
void          engine_set_object_name(int obj_id, const uint8_t *name, int len);
const uint8_t *engine_get_object_name(int obj_id);

// VAR-driven flag — ScummVM tracks the "talk-actor" via _talkingActor,
// queried by VAR_TALK_ACTOR. We expose the set/get hooks the talk
// pipeline needs.
void     engine_set_talking_actor(int actor_num);

// Camera control — mirrors ScummEngine setCameraAt / panCameraTo /
// setCameraFollows. Called from the matching opcode handlers.
void engine_camera_set_at(int pos_x);
void engine_camera_pan_to(int x);
void engine_camera_set_follows(int actor_num, bool force);

}  // namespace tsb
