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
// Mirrors Actor::MF_FROZEN (actor.h:54). Set by freezeActors / cleared
// by unfreezeActors. While set, walkActor / animateCostume become no-ops.
constexpr int MOVE_FROZEN   = 0x80;

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
    // Actor::_width — default 24 (ScummVM actor.cpp:195 initActor). Used
    // by walkActorToActor distance and centred talk-text placement.
    uint8_t  width;

    uint8_t  flags;
    uint8_t  layer;
    uint8_t  force_clip;
    // ScummVM Actor::_walkScript / _talkScript — script numbers invoked
    // by startWalkAnim / runActorTalkScript. 0 = default (built-in
    // setDirection / startAnimActor path).
    uint16_t walk_script;
    uint16_t talk_script;

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
// Direct port of scummvm Actor::putActor(int x, int y, int newRoom)
// (actor.cpp:1730-1782). actor_put_at and actor_put_in_room are the
// 2-arg and 1-arg-room shorthands that funnel here, matching the
// scummvm header overloads (actor.h:213-225).
void   actor_put_actor(int actor_num, int x, int y, int new_room);
void   actor_put_at(int actor_num, int x, int y);
void   actor_put_in_room(int actor_num, int room);
void   actor_hide(int actor_num);
void   actor_hide_all();
void   actor_show_in_current_room(int current_room);

// Mirrors scummvm `_egoPositioned`. Set true by actor_put_at when the
// actor being placed IS the ego (VAR_EGO). loadRoomWithEgo clears it
// before running ENCD; if ENCD positions the ego, the flag becomes
// true and the v4 "walk-pos fallback" is skipped. Without this we
// always teleport the ego to the object's walk-pos, overriding
// whatever the entry script set.
bool   actor_ego_positioned_get();
void   actor_ego_positioned_set(bool v);
void   actor_set_costume(int actor_num, int costume_id);
void   actor_walk_to(int actor_num, int x, int y);
void   actor_animate(int actor_num, int anim);
void   actor_face_object(int actor_num, int object);
void   actor_set_facing(int actor_num, int direction);  // setDirection

// Mirrors Actor::startAnimActor (actor.cpp:2692-2759). Decodes the
// chore redirect sentinels (CHORE_REDIRECT_INIT/WALK/STAND/...), then
// runs costumeDecodeData(frame, ~0) to refresh per-limb state. Also
// resets _cost on initFrame.
void   actor_start_anim(int actor_num, int frame);

// Mirrors Actor::animateActor (actor.cpp:2817-2874): chore = anim>>2,
// dir = oldDirToNewDir(anim & 3). Chore 2 = stop walking, 3 =
// setDirection, 4 = turnToDirection, default = startAnimActor.
void   actor_animate_chore(int actor_num, int anim);
// Re-init one actor — mirrors ScummEngine::Actor::initActor with `mode`:
//   -1 = full reset (used at game-startup);
//    0 = soft reset (preserves room/cost/pos/facing — o5_actorOps SO_DEFAULT);
//    1 = same as -1 but skips animVariable (rare, used by setOwnerOf).
void   actor_init_one(int actor_num, int mode);

// Class-data flip for o5_setClass (object.cpp::putClass action on
// actors: if the object is in actor range, classChanged toggles
// _ignoreBoxes / _forceClip flags). Mirrors actor.cpp::classChanged.
void   actor_class_changed(int actor_num, int cls, bool set);

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
