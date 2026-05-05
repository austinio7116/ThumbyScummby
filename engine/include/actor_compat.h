// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — actor free-function compatibility layer.
//
// The transcribed scummvm Actor class lives in actor.h.  Our remaining
// hand-written engine code (engine.cpp, opcodes.cpp, etc.) was written
// against the OLD free-function API (`actor_get`, `actor_init_one`, ...).
// This header preserves that API as a thin wrapper around the transcribed
// methods so we don't have to migrate every caller in a single commit.
//
// As each upstream caller-site file gets transcribed (script_v5.cpp etc.),
// these wrappers are deleted and callers go through `g_scumm->_actors[n]`
// / `g_scumm->derefActor(n)` directly.

#pragma once

#include "scummvm_compat.h"
#include "actor.h"

namespace tsb {

Actor *actor_get(int actor_num);

void actor_init_all();
void actor_init_one(int actor_num, int mode);

void actor_set_costume(int actor_num, int costume_id);
void actor_walk_to(int actor_num, int x, int y);
void actor_start_walk(int actor_num, int dst_x, int dst_y, int dir);
void actor_animate(int actor_num, int anim);
void actor_animate_chore(int actor_num, int anim);
void actor_face_object(int actor_num, int object);
void actor_set_facing(int actor_num, int direction);
void actor_start_anim(int actor_num, int frame);
void actor_class_changed(int actor_num, int cls, bool set);

void actor_put_actor(int actor_num, int x, int y, int new_room);
void actor_put_at(int actor_num, int x, int y);
void actor_put_in_room(int actor_num, int room);

void actor_hide(int actor_num);
void actor_hide_all();
void actor_show_in_current_room(int current_room);

void actor_adjust_pos(Actor *a);

bool actor_ego_positioned_get();
void actor_ego_positioned_set(bool v);

// Per-frame walking + costume render — wraps the transcribed walkActors /
// animateCostumes pair plus our existing legacy renderer.
struct WalkboxGraph;     // forward — defined in legacy walkbox.h
void actor_tick_all(const WalkboxGraph *wbg);
void actor_render_all(uint8_t *vscreen_main, int pitch,
                      const WalkboxGraph *wbg, int x_off);

}  // namespace tsb
