// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — actor state and operations.
//
// Walking, animation, costume render. Walking uses 16.16 fixed-point
// fractional accumulators (matches SCUMM v4/v5 disasm — see
// scummvm-upstream/engines/scumm/actor.cpp:520-682).

#pragma once

#include "types.h"
#include "walkbox.h"

namespace tsb {

constexpr int ACTOR_FLAG_VISIBLE     = 0x01;
constexpr int ACTOR_FLAG_IGNORE_BOX  = 0x02;
constexpr int ACTOR_FLAG_NEVER_ZCLIP = 0x04;
constexpr int ACTOR_FLAG_FORCE_ZCLIP = 0x08;
constexpr int ACTOR_FLAG_FLIP_X      = 0x10;

constexpr int MOVE_NEW_LEG  = 1;
constexpr int MOVE_IN_LEG   = 2;
constexpr int MOVE_TURN     = 4;
constexpr int MOVE_LAST_LEG = 8;

// Per-limb costume animation state. Mirrors ScummVM CostumeData (one slot
// per limb). 0xFFFF in `frame` / `curpos` means "limb empty/stopped".
struct ActorCostumeAnim {
    uint8_t  anim_type[16];
    uint16_t curpos[16];
    uint16_t start[16];
    uint16_t end[16];
    uint16_t frame[16];
    uint16_t stopped_mask;
};

struct Actor {
    uint8_t  number;
    uint8_t  room;
    uint16_t costume;

    int16_t  x, y;
    int16_t  elevation;

    uint16_t facing;
    uint16_t target_facing;
    uint8_t  moving;
    uint8_t  walkbox;
    uint16_t speedx, speedy;

    uint8_t  frame;
    uint8_t  init_frame, walk_frame, stand_frame;
    uint8_t  talk_start_frame, talk_stop_frame;
    uint8_t  anim_speed;
    uint8_t  anim_progress;

    uint8_t  scalex, scaley;
    uint8_t  talk_color;
    uint8_t  shadow_mode;
    int16_t  talk_pos_x, talk_pos_y;

    uint8_t  flags;
    uint8_t  layer;
    uint8_t  force_clip;

    // Walk destination & current-leg waypoint
    int16_t  dest_x, dest_y;
    uint8_t  dest_box;
    uint8_t  cur_box;       // box the next-leg starts in

    int16_t  next_x, next_y;     // waypoint for current leg
    int32_t  delta_x, delta_y;   // 16.16 per-frame increments (fixed point)
    int32_t  xfrac, yfrac;       // 16.16 sub-pixel accumulator

    // Costume per-limb animation
    ActorCostumeAnim cost;

    // Per-actor palette remap (32 entries — costumes use up to 32 colors)
    uint8_t  palette[32];
};

void actor_init_all();
Actor *actor_get(int actor_num);  // bounds-checked; returns nullptr on bad id
void   actor_put_at(int actor_num, int x, int y);
void   actor_put_in_room(int actor_num, int room);
void   actor_set_costume(int actor_num, int costume_id);
void   actor_walk_to(int actor_num, int x, int y);
void   actor_animate(int actor_num, int anim);
void   actor_face_object(int actor_num, int object);

// Per-frame walking + costume render hooks called from the engine main loop.
// `wbg` is the current room's walkbox graph (may be nullptr if the room has
// no walkboxes — e.g. inventory/title screens).
void   actor_tick_all(const WalkboxGraph *wbg);
// Renders all visible actors. `x_off` is subtracted from each actor's
// draw position, mapping room x → viewport x. Mirrors the way ScummVM
// processActors writes to the main VirtScreen using positions relative
// to camera/_screenStartStrip.
void   actor_render_all(uint8_t *vscreen_main, int pitch,
                        const WalkboxGraph *wbg, int x_off = 0);

}  // namespace tsb
