// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — actor walk + render.
//
// Per-frame:
//  1. tick: advance walking using 16.16 fixed-point step accumulator,
//     consult walkbox graph for next-box on path. Cycle animation frame
//     timer.
//  2. render: Y-sort visible actors; for each, look up costume, walk
//     through its limbs and call costume_render_limb at the actor's
//     position.
//
// This is the v4/v5 small-header path. Many of the subtle ScummVM
// behaviours (smooth turning, walk-script triggers, sound triggers) are
// stubbed; we implement enough for actors to appear and move.

#include "actor.h"
#include "costume.h"
#include "walkbox.h"
#include "resource.h"
#include "object.h"
#include "platform.h"
#include "vm.h"
#include "engine.h"

#include <string.h>
#include <stdlib.h>

namespace tsb {

// Provided by engine.cpp — pointer to the current room's WalkboxGraph,
// or nullptr if the room has no walkboxes (also used by walkbox.cpp).
extern WalkboxGraph *engine_active_walkbox_graph();

static Actor g_actors[MAX_ACTORS];

// ---------------------------------------------------------------------------
// Initialization & simple setters
// ---------------------------------------------------------------------------

// Mirrors ScummEngine::Actor::initActor (actor.cpp:161-235). `mode == -1`
// is the full reset done at game startup; `mode == 0` is the soft reset
// invoked by o5_actorOps SO_DEFAULT (preserves room/cost/pos/facing).
void actor_init_one(int n, int mode) {
    if (n < 0 || n >= MAX_ACTORS) return;
    Actor &a = g_actors[n];

    if (mode == -1) {
        a.number = (uint8_t)n;
        a.flags  = 0;
        a.moving = 0;
        a.frame  = 0;
        // ScummVM Actor::initActor (audio/adlib.cpp wait — actor.cpp:173)
        // sets _walkbox = 0 (NOT kInvalidBox=0xFF). This affects z-mask
        // selection: getMaskFromBox(0) returns box 0's mask byte, which
        // for v4 MI1 floppy intro typically routes clouds onto z-plane
        // 1 (behind cliff peak). We previously initialised to 0xFF
        // which made getMaskFromBox(255) return 0 → no clip → clouds
        // rendered on top of the cliff.
        a.walkbox = 0;
        a.cur_box = INVALID_BOX;
        a.dest_box = INVALID_BOX;
        a.anim_progress = 0;
        for (int p = 0; p < 32; p++) a.palette[p] = 0;
        // Initialise costume anim slots to "empty" — Actor::_cost.reset()
        // sets every curpos[i] to 0xFFFF and stopped to 0xFFFF.
        for (int l = 0; l < 16; l++) {
            a.cost.anim_type[l] = 0;
            a.cost.curpos[l]    = 0xFFFF;
            a.cost.start[l]     = 0xFFFF;
            a.cost.end[l]       = 0xFFFF;
            a.cost.frame[l]     = 0xFFFF;
        }
        a.cost.stopped_mask = 0;
        a.walk_script = 0;
    }

    if (mode == 1 || mode == -1) {
        a.costume = 0;
        a.room    = 0;
        a.x       = 0;
        a.y       = 0;
        a.facing  = 180;
    } else if (mode == 2) {
        a.facing  = 180;
    }
    a.elevation        = 0;
    a.width            = 24;                   // actor.cpp:195
    a.talk_color       = 15;
    a.talk_pos_x       = 0;
    a.talk_pos_y       = -80;
    a.scalex = a.scaley = 0xFF;
    a.target_facing    = a.facing;

    a.shadow_mode      = 0;
    a.layer            = 0;

    a.moving           = 0;        // stopActorMoving — clear walk state

    // Walk speed defaults via setActorWalkSpeed(8, 2) — actor.cpp:211
    a.speedx = 8;
    a.speedy = 2;

    a.anim_speed       = 0;

    a.flags &= ~ACTOR_FLAG_IGNORE_BOX;     // _ignoreBoxes = false
    a.force_clip       = 0;

    // Default per-anim frame slots — actor.cpp:225-229
    a.init_frame       = 1;
    a.walk_frame       = 2;
    a.stand_frame      = 3;
    a.talk_start_frame = 4;
    a.talk_stop_frame  = 5;

    a.walk_script      = 0;
    a.talk_script      = 0;
}

void actor_init_all() {
    memset(g_actors, 0, sizeof(g_actors));
    for (int i = 0; i < MAX_ACTORS; i++) {
        actor_init_one(i, -1);
        // Default palette after initActor(-1): 0xFF in every entry —
        // sentinel "use the costume's own palette[i]". Mirrors
        // setActorCostume (actor.cpp:3702-3703) for v4/v5 (non-GF_OLD_BUNDLE):
        //     for (i = 0; i < 32; i++) _palette[i] = 0xFF;
        // op_actorOps sub-op 11 (SO_PALETTE) overrides specific entries.
        for (int c = 0; c < 32; c++) g_actors[i].palette[c] = 0xFF;
    }
}

// Mirrors Actor::classChanged (actor.cpp). For SCUMM v4/v5 the only
// classes that affect actor state are kObjectClassNeverClip(20) /
// kObjectClassAlwaysClip(21) / kObjectClassIgnoreBoxes(22). Higher-numbered
// classes (XFlip/YFlip/Player/Untouchable) are the ones remapped to the
// SMALL_HEADER 18..24 range by putClass.
void actor_class_changed(int n, int cls, bool set) {
    Actor *a = actor_get(n);
    if (!a) return;
    switch (cls) {
    case 21:    // kObjectClassAlwaysClip — scummvm Actor::classChanged
        if (set) { a->flags |= ACTOR_FLAG_FORCE_ZCLIP; a->force_clip = 1; }
        else     { a->flags &= ~ACTOR_FLAG_FORCE_ZCLIP; a->force_clip = 0; }
        break;
    case 22:    // kObjectClassIgnoreBoxes — scummvm Actor::classChanged
        if (set) a->flags |=  ACTOR_FLAG_IGNORE_BOX;
        else     a->flags &= ~ACTOR_FLAG_IGNORE_BOX;
        break;
    case 30:    // kObjectClassXFlip
        if (set) a->flags |=  ACTOR_FLAG_FLIP_X;
        else     a->flags &= ~ACTOR_FLAG_FLIP_X;
        break;
    default: break;
    }
}

// Mirrors ScummEngine::Actor::setDirection (actor.cpp:1577-1623). For
// version <= 6 this updates _facing and (when costume is set) walks the
// per-limb _cost.frame[] table to redecode each non-empty limb in the
// new facing.
void actor_set_facing(int n, int direction) {
    Actor *a = actor_get(n); if (!a) return;
    direction = ((direction % 360) + 360) % 360;
    if (a->facing == (uint16_t)direction) return;
    a->facing = (uint16_t)direction;
    a->target_facing = (uint16_t)direction;
    if (a->costume == 0) return;

    // costume.cpp:1599-1620: walk all 16 limbs; for any non-empty
    // _cost.frame[i], if the encoded direction (v <= 6: low 2 bits of
    // _cost.frame[i]) doesn't match newDirToOldDir(_facing), redecode
    // with usemask = aMask (single-bit). This refreshes cels for
    // facing-aware limbs.
    int new_dir = costume_new_dir_to_old(direction);
    unsigned aMask = 0x8000;
    for (int i = 0; i < 16; i++, aMask >>= 1) {
        uint16_t vald = a->cost.frame[i];
        if (vald == 0xFFFF) continue;
        // GF_NEW_COSTUMES is false for v4/v5 — store direction in the
        // frame field's low 2 bits (actor.cpp:1610).
        if (((int)vald & 3) == new_dir) continue;
        // Decode using only this limb (aMask)
        costume_decode_data(a, vald >> 2, aMask);
    }
}

// Direct port of Actor::startAnimActor (actor.cpp:2692-2759).
// CHORE_REDIRECT_* are decimal 56..60 in scummvm-upstream/actor.h:32-36 —
// NOT 0xFA..0xFE as we previously had. The decode is gated on
// isInCurrentRoom() && _costume != 0, and only the _cost.reset() (limb
// clear) happens when frame == _initFrame.
void actor_start_anim(int n, int frame) {
    Actor *a = actor_get(n); if (!a) return;

    switch (frame) {
    case 56: frame = a->init_frame; break;       // CHORE_REDIRECT_INIT
    case 57: frame = a->walk_frame; break;       // CHORE_REDIRECT_WALK
    case 58: frame = a->stand_frame; break;      // CHORE_REDIRECT_STAND
    case 59: frame = a->talk_start_frame; break; // CHORE_REDIRECT_START_TALK
    case 60: frame = a->talk_stop_frame; break;  // CHORE_REDIRECT_STOP_TALK
    default: break;
    }

    int current_room = engine_current_room_id();
    bool in_current = ((int)a->room == current_room);
    if (in_current && a->costume != 0) {
        a->anim_progress = 0;
        // _cost.animCounter = 0 — we don't track this field.
        if (frame == a->init_frame) {
            // CostumeData::reset() (actor.h:78-89): every limb empty,
            // stopped = 0 (NOT 0xFFFF — limbs are stopped/unstopped by
            // decode commands 0x79 / 0x7A).
            for (int l = 0; l < 16; l++) {
                a->cost.curpos[l] = 0xFFFF;
                a->cost.start[l]  = 0xFFFF;
                a->cost.end[l]    = 0xFFFF;
                a->cost.frame[l]  = 0xFFFF;
            }
            a->cost.stopped_mask = 0;
        }
        costume_decode_data(a, frame, (unsigned)-1);
        a->frame = (uint8_t)frame;
    }
}

// Mirrors Actor::animateActor (actor.cpp:2817-2874).
//   chore = anim >> 2; dir = oldDirToNewDir(anim & 3).
//   case 2 = stop walking + startAnimActor(_standFrame).
//   case 3 = setDirection (clear MF_TURN).
//   case 4 = turnToDirection.
//   default = startAnimActor(anim).
void actor_animate_chore(int n, int anim) {
    Actor *a = actor_get(n); if (!a) return;
    int chore = anim >> 2;
    static const int new_dir_table[4] = { 270, 90, 180, 0 };
    int dir = new_dir_table[anim & 3];
    // Convert "old chore code" — actor.cpp:2836: chore = 0x3F - chore + 2.
    chore = 0x3F - chore + 2;
    switch (chore) {
    case 2: // stop walking
        actor_start_anim(n, a->stand_frame);
        a->moving = 0;
        break;
    case 3: // change direction immediately
        actor_set_facing(n, dir);
        break;
    case 4: // turn to direction (smooth)
        a->target_facing = (uint16_t)dir;
        break;
    default:
        actor_start_anim(n, anim);
        break;
    }
}

Actor *actor_get(int n) {
    if (n < 0 || n >= MAX_ACTORS) return nullptr;
    return &g_actors[n];
}

// Mirrors scummvm `_egoPositioned` (one global flag per engine).
static bool g_ego_positioned = false;
bool actor_ego_positioned_get() { return g_ego_positioned; }
void actor_ego_positioned_set(bool v) { g_ego_positioned = v; }

// Mirror of Actor::setBox (actor.cpp:416): _walkbox = box; setupActorScale().
// We forward-declare setup_actor_scale below so the caller compiles; the
// scale recompute happens once per setBox to match scummvm exactly.
static void setup_actor_scale(Actor *a, const WalkboxGraph *wbg);
static void actor_set_box(Actor *a, int box) {
    a->walkbox = (uint8_t)box;
    setup_actor_scale(a, engine_active_walkbox_graph());
}

// Direct port of Actor::adjustActorPos (actor.cpp:2090-2113).
// Snaps the actor onto the closest walkbox after a putActor in the
// current room, then sets walkdata.destbox / walkbox / kicks off a
// turnToDirection if the box has any "follow boxes" flag bits set.
void actor_adjust_pos(Actor *a) {
    int adj_x = a->x, adj_y = a->y;
    WalkboxGraph *wbg = engine_active_walkbox_graph();
    uint8_t adj_box = walkbox_adjust_xy(wbg, a->x, a->y, &adj_x, &adj_y);

    a->x = (int16_t)adj_x;
    a->y = (int16_t)adj_y;
    a->dest_box = adj_box;

    actor_set_box(a, adj_box);

    a->dest_x = -1;     // mirrors _walkdata.dest.x = -1

    // stopActorMoving: clear walk state, stop walk script if any.
    if (a->walk_script) vm_stop_script(&g_vm, a->walk_script);
    a->moving = 0;
    // _cost.soundCounter = 0; _cost.soundPos = 0  (we don't track these.)

    // If the resulting walkbox has a flag (mirroring scummvm's
    // `getBoxFlags(_walkbox) & 7`), kick off a turn. We don't model
    // turnToDirection's smooth-turn, so this is a no-op for now —
    // actors will face their default facing.
}

// Direct port of scummvm Actor::putActor(int dstX, int dstY, int newRoom)
// (actor.cpp:1730-1782). All other putActor variants funnel here, which
// is why scummvm gets the visibility / room transitions right and we
// were getting them wrong by hand-rolling smaller wrappers.
void actor_put_actor(int n, int x, int y, int new_room) {
    Actor *a = actor_get(n); if (!a) return;

    // (Talking-actor stopTalk skipped — out of scope for us.)

    a->x = (int16_t)x;
    a->y = (int16_t)y;
    a->room = (uint8_t)new_room;

    if ((int)g_vm.globals[VAR_EGO] == n) {
        g_ego_positioned = true;
    }

    int current_room = engine_current_room_id();
    bool in_current = ((int)a->room == current_room);
    bool was_visible = (a->flags & ACTOR_FLAG_VISIBLE) != 0;

    if (was_visible) {
        if (in_current) {
            // scummvm: if (_moving) { stopActorMoving(); startAnimActor(_standFrame); }
            // adjustActorPos();
            if (a->moving) {
                a->moving = 0;
                actor_start_anim(n, a->stand_frame);
            }
            actor_adjust_pos(a);
        } else {
            // hideActor (actor.cpp:2174-2187): clear _visible, stop moving.
            a->flags &= (uint8_t)~ACTOR_FLAG_VISIBLE;
            a->moving = 0;
        }
    } else {
        if (in_current) {
            // showActor (actor.cpp:2198-2241): only acts if _currentRoom != 0.
            // For v3+ : adjustActorPos, ensureResourceLoaded(rtCostume, _costume),
            // if (_costumeNeedsInit) startAnimActor(_initFrame), stopActorMoving,
            // _visible = true.  We don't track _costumeNeedsInit explicitly —
            // setActorCostume's path already startAnimActor's on visible
            // transitions, and showActor here picks up actors whose costume
            // was set while invisible.
            actor_adjust_pos(a);
            a->flags |= ACTOR_FLAG_VISIBLE;
            a->moving = 0;
            if (a->costume != 0) {
                actor_start_anim(n, a->init_frame);
            }
        }
    }
}

// 2-arg form: scummvm `void putActor(int x, int y) { putActor(x, y, _room); }`
void actor_put_at(int n, int x, int y) {
    Actor *a = actor_get(n); if (!a) return;
    actor_put_actor(n, x, y, a->room);
}

// 1-arg-room form: scummvm `void putActor(int room) { putActor(_pos.x, _pos.y, room); }`
void actor_put_in_room(int n, int room) {
    Actor *a = actor_get(n); if (!a) return;
    actor_put_actor(n, a->x, a->y, room);
}

// Mirror of scummvm Actor::hideActor (actor.cpp:1002): clears the visible
// flag, stops walking. Called for every actor at room change so the new
// room starts with a clean actor slate; entry scripts (ENCD / Script 5)
// then put back the actors that belong here.
void actor_hide(int n) {
    Actor *a = actor_get(n); if (!a) return;
    a->flags &= (uint8_t)~ACTOR_FLAG_VISIBLE;
    a->moving = 0;
}

void actor_hide_all() {
    for (int i = 0; i < MAX_ACTORS; i++) {
        Actor &a = g_actors[i];
        a.flags &= (uint8_t)~ACTOR_FLAG_VISIBLE;
        a.moving = 0;
    }
}

// Mirror of ScummEngine::showActors + Actor::showActor
// (scummvm-upstream/actor.cpp:2198-2241, 2243-2250). For each actor
// already in the current room, run adjustActorPos, kick off the init
// frame, mark visible, stop moving. Direct port — earlier hand-rolled
// variant only set the visible flag, missing the box snap that
// setupActorScale relies on.
void actor_show_in_current_room(int current_room) {
    if (current_room == 0) return;
    for (int i = 0; i < MAX_ACTORS; i++) {
        Actor &a = g_actors[i];
        if ((int)a.room != current_room) continue;
        if (a.flags & ACTOR_FLAG_VISIBLE) continue;     // already visible
        actor_adjust_pos(&a);
        if (a.costume != 0) {
            // ensureResourceLoaded skipped — our resource path is
            // disk-mapped, no separate cache.
            actor_start_anim(i, a.init_frame);
        }
        a.flags |= ACTOR_FLAG_VISIBLE;
        a.moving = 0;
    }
}

// Direct port of Actor::setActorCostume (scummvm-upstream/actor.cpp:3661).
// For v4 MI1 (non-GF_NEW_COSTUMES): if visible, hideActor + reset _cost +
// set new costume + showActor (so adjustActorPos and the init-frame anim
// fire as a single transaction). If invisible, just set the field —
// the next showActor (room enter) will handle the rest.
void actor_set_costume(int n, int cost) {
    Actor *a = actor_get(n); if (!a) return;

    // _costumeNeedsInit = true — we don't track it directly; the equivalent
    // is that showActor (called below) always startAnimActor(_initFrame).

    auto reset_cost = [a]() {
        a->cost.stopped_mask = 0;
        for (int l = 0; l < 16; l++) {
            a->cost.curpos[l] = 0xFFFF;
            a->cost.start[l]  = 0xFFFF;
            a->cost.end[l]    = 0xFFFF;
            a->cost.frame[l]  = 0xFFFF;
        }
    };

    if (a->flags & ACTOR_FLAG_VISIBLE) {
        // hideActor: stopMoving + (was-moving → standFrame) + clear visible.
        if (a->moving) {
            a->moving = 0;
            actor_start_anim(n, a->stand_frame);
        }
        a->flags &= (uint8_t)~ACTOR_FLAG_VISIBLE;

        reset_cost();
        a->costume = (uint16_t)cost;

        // showActor: adjustActorPos, startAnimActor(_initFrame), visible = true.
        actor_adjust_pos(a);
        a->flags |= ACTOR_FLAG_VISIBLE;
        a->moving = 0;
        if (cost != 0) {
            actor_start_anim(n, a->init_frame);
        }
    } else {
        a->costume = (uint16_t)cost;
        reset_cost();
    }

    // Default palette (non-GF_OLD_BUNDLE): all 32 entries 0xFF (sentinel —
    // costume's own palette[i] is used).  scummvm-upstream/actor.cpp:3702.
    for (int p = 0; p < 32; p++) a->palette[p] = 0xFF;
    a->anim_progress = 0;
}

// Direct port of Actor::startWalkActor (scummvm-upstream/actor.cpp:850-917).
// v4 path: skip adjustXYToBeInBox (game uses the destination as-is — the
// pathing inside walkActor will route through walkboxes), then arm the
// walkdata fields and set _moving = MF_NEW_LEG.
void actor_walk_to(int n, int x, int y) {
    actor_start_walk(n, x, y, /*dir=*/-1);     // see header for signature
}

void actor_start_walk(int n, int dst_x, int dst_y, int dir) {
    Actor *a = actor_get(n); if (!a) return;

    // v4: AdjustBoxResult is just (destX, destY, kInvalidBox) — the
    // routing happens later in walkActor. (v5+ would call adjustXYToBeInBox here.)
    int abr_x = dst_x;
    int abr_y = dst_y;
    uint8_t abr_box = INVALID_BOX;

    int current_room = engine_current_room_id();
    bool in_current = ((int)a->room == current_room);

    // For v4-6: if not in current room, just teleport and set facing.
    if (!in_current) {
        a->x = (int16_t)abr_x;
        a->y = (int16_t)abr_y;
        // _ignoreTurns isn't tracked; assume false for v4 MI1.
        if (dir != -1) a->facing = (uint16_t)dir;
        return;
    }

    if (a->flags & ACTOR_FLAG_IGNORE_BOX) {
        abr_box = INVALID_BOX;
        a->walkbox = INVALID_BOX;
    } else {
        // checkXYInBoxBounds(_walkdata.destbox, abr.x, abr.y) — if the
        // existing destbox still contains the new (x,y), reuse it; else
        // re-snap.
        WalkboxGraph *wbg = engine_active_walkbox_graph();
        if (wbg && wbg->valid && a->dest_box != INVALID_BOX &&
            walkbox_xy_in_box(wbg, a->dest_box, abr_x, abr_y)) {
            abr_box = a->dest_box;
        } else {
            int adj_x = abr_x, adj_y = abr_y;
            abr_box = walkbox_adjust_xy(wbg, abr_x, abr_y, &adj_x, &adj_y);
            abr_x = adj_x; abr_y = adj_y;
        }

        // If we were already heading to the same destination + dir, no-op.
        if (a->moving && (int)a->walk_dest_dir == dir &&
            a->dest_x == abr_x && a->dest_y == abr_y) {
            return;
        }
    }

    if (a->x == abr_x && a->y == abr_y) {
        // Already at destination — turn-only.
        if (dir != -1 && (int)a->facing != dir) {
            a->target_facing = (uint16_t)dir;
            a->moving |= MOVE_TURN;
        }
        return;
    }

    a->dest_x = (int16_t)abr_x;
    a->dest_y = (int16_t)abr_y;
    a->dest_box = abr_box;
    a->walk_dest_dir = (int16_t)dir;
    a->point3_x = 32000;        // sentinel
    a->cur_box = a->walkbox;

    // v3+: _moving = (_moving & MF_IN_LEG) | MF_NEW_LEG.
    a->moving = (a->moving & MOVE_IN_LEG) | MOVE_NEW_LEG;
}

// Mirrors o5_animateActor -> Actor::animateActor (actor.cpp:2817-2874).
void actor_animate(int n, int anim) {
    actor_animate_chore(n, anim);
}

// Mirrors Actor::faceToObject (scummvm-upstream/engines/scumm/actor.cpp:1651).
// v4 picks one of four cardinal directions based on which axis has the
// larger delta to the target; we encode the result directly in our 0..359°
// facing convention (0=down, 90=right, 180=up, 270=left) instead of the
// upstream 4-bit `oldDir` -> `newDir` indirection.
void actor_face_object(int n, int object) {
    Actor *a = actor_get(n);
    if (!a || a->room == 0) return;

    extern ObjectTable *get_object_table();
    ObjectTable *t = get_object_table();
    if (!t) return;
    ObjectData *od = object_get_by_id(t, object);
    if (!od) return;

    int ox = od->x_strip * 8 + (od->w_strip * 8) / 2;
    int oy = od->y * 8 + (od->h * 8) / 2;
    int dx = ox - a->x;
    int dy = oy - a->y;

    int dir;
    if ((dx >= 0 ? dx : -dx) >= (dy >= 0 ? dy : -dy)) {
        dir = (dx >= 0) ? 90 : 270;
    } else {
        dir = (dy >= 0) ? 0 : 180;
    }
    actor_set_facing(n, dir);
}

// ---------------------------------------------------------------------------
// Walking — direct ports of scummvm Actor::calcMovementFactor /
// Actor::actorWalkStep / Actor::walkActor / Actor::startWalkAnim.
// ---------------------------------------------------------------------------

// Mirrors Actor::startWalkAnim (scummvm-upstream/actor.cpp:919-943).
//   cmd 1 = start walk  (setDirection + startAnimActor(walkFrame))
//   cmd 3 = stop walk   (turnToDirection + startAnimActor(standFrame))
// We're v4 ≤ 6, so always setDirection (not turnToDirection) for cmd != 3.
static void start_walk_anim(Actor *a, int cmd, int angle) {
    if (angle == -1) angle = (int)a->facing;

    if (a->walk_script) {
        int32_t args[3] = { (int32_t)a->number, (int32_t)cmd, (int32_t)angle };
        vm_start_script(&g_vm, a->walk_script, args, 3, false, false);
        return;
    }
    if (cmd == 3) {
        // turnToDirection — for our 4-direction encoding, just set facing.
        actor_set_facing((int)a->number, angle);
    } else {
        actor_set_facing((int)a->number, angle);
    }
    if (cmd == 1)      actor_start_anim((int)a->number, a->walk_frame);
    else if (cmd == 3) actor_start_anim((int)a->number, a->stand_frame);
}

// Mirrors Actor::updateActorDirection (scummvm-upstream/actor.cpp:1544).
// For v4-6 with 4-direction encoding the result is just the requested
// target direction — there's no costume-direction-count remap step.
static int update_actor_direction(Actor *a, bool /*is_walking*/) {
    return (int)a->target_facing;
}

// Direct port of Actor::calcMovementFactor (scummvm-upstream/actor.cpp:520-576).
// Returns the result of actorWalkStep() so the first step happens in the
// same call (mirrors scummvm).
static int actor_walk_step(Actor *a);
static int actor_calc_movement_factor(Actor *a, int next_x, int next_y) {
    int diffX, diffY;
    int32_t deltaXFactor, deltaYFactor;

    if (a->x == next_x && a->y == next_y)
        return 0;

    diffX = next_x - a->x;
    diffY = next_y - a->y;

    deltaYFactor = (int32_t)a->speedy << 16;
    if (diffY < 0) deltaYFactor = -deltaYFactor;

    deltaXFactor = deltaYFactor * diffX;
    if (diffY != 0) {
        deltaXFactor /= diffY;
    } else {
        deltaYFactor = 0;
    }

    // For SCUMM4-6: the disasm divides by 0x10000 (not abs(>>16)).
    int absDX = (deltaXFactor / 0x10000); if (absDX < 0) absDX = -absDX;
    if ((unsigned)absDX > a->speedx) {
        deltaXFactor = (int32_t)a->speedx << 16;
        if (diffX < 0) deltaXFactor = -deltaXFactor;
        deltaYFactor = deltaXFactor * diffY;
        if (diffX != 0) deltaYFactor /= diffX;
        else            deltaXFactor = 0;
    }

    a->xfrac = 0;
    a->yfrac = 0;
    a->cur_x = a->x; a->cur_y = a->y;
    a->next_x = (int16_t)next_x; a->next_y = (int16_t)next_y;
    a->delta_x = deltaXFactor;
    a->delta_y = deltaYFactor;

    // v4-6: _targetFacing = (ABS(diffY)*3 > ABS(diffX)) ? (deltaYFactor>0?180:0) : (deltaXFactor>0?90:270)
    int absDiffX = diffX < 0 ? -diffX : diffX;
    int absDiffY = diffY < 0 ? -diffY : diffY;
    a->target_facing = (uint16_t)((absDiffY * 3 > absDiffX)
        ? (deltaYFactor > 0 ? 180 : 0)
        : (deltaXFactor > 0 ? 90 : 270));

    return actor_walk_step(a);
}

// Direct port of Actor::actorWalkStep (scummvm-upstream/actor.cpp:633-682).
static int actor_walk_step(Actor *a) {
    // _needRedraw = true (we redraw every frame anyway).

    // v4-6 path: updateActorDirection(true) → setDirection if changed +
    // startWalkAnim(1, nextFacing) on first transition into MF_IN_LEG.
    int nextFacing = update_actor_direction(a, true);
    if ((a->walk_frame != a->frame && !(a->moving & MOVE_IN_LEG)) ||
        (int)a->facing != nextFacing) {
        start_walk_anim(a, 1, nextFacing);
    }
    a->moving |= MOVE_IN_LEG;

    WalkboxGraph *wbg = engine_active_walkbox_graph();
    if (a->walkbox != a->cur_box && wbg && wbg->valid &&
        a->cur_box != INVALID_BOX &&
        walkbox_xy_in_box(wbg, a->cur_box, a->x, a->y)) {
        actor_set_box(a, a->cur_box);
    }

    int distX = a->next_x - a->cur_x; if (distX < 0) distX = -distX;
    int distY = a->next_y - a->cur_y; if (distY < 0) distY = -distY;

    int absPosCurX = a->x - a->cur_x; if (absPosCurX < 0) absPosCurX = -absPosCurX;
    int absPosCurY = a->y - a->cur_y; if (absPosCurY < 0) absPosCurY = -absPosCurY;
    if (absPosCurX >= distX && absPosCurY >= distY) {
        a->moving &= ~MOVE_IN_LEG;
        return 0;
    }

    // v4-6 fixed-point step:
    //   tmpX = pos.x * 0x10000 + xfrac + (deltaXFactor >> 8) * scalex
    //   xfrac = (uint16) tmpX
    //   pos.x = tmpX >> 16
    int32_t tmpX = (int32_t)(a->x * 0x10000) + (int32_t)a->xfrac +
                   (int32_t)((a->delta_x >> 8) * a->scalex);
    a->xfrac = (uint16_t)tmpX;
    a->x = (int16_t)(tmpX >> 16);

    int32_t tmpY = (int32_t)(a->y * 0x10000) + (int32_t)a->yfrac +
                   (int32_t)((a->delta_y >> 8) * a->scaley);
    a->yfrac = (uint16_t)tmpY;
    a->y = (int16_t)(tmpY >> 16);

    int absPosCurX2 = a->x - a->cur_x; if (absPosCurX2 < 0) absPosCurX2 = -absPosCurX2;
    int absPosCurY2 = a->y - a->cur_y; if (absPosCurY2 < 0) absPosCurY2 = -absPosCurY2;
    if (absPosCurX2 > distX) a->x = a->next_x;
    if (absPosCurY2 > distY) a->y = a->next_y;

    if (a->x == a->next_x && a->y == a->next_y) {
        a->moving &= ~MOVE_IN_LEG;
        return 0;
    }

    return 1;
}

// Direct port of Actor::walkActor (scummvm-upstream/actor.cpp:945-1015).
// One-shot per-frame dispatcher: handles MF_TURN, MF_LAST_LEG, MF_NEW_LEG,
// runs the box-routing loop, finally calls calcMovementFactor for the next
// leg's gate.
static void walk_actor(Actor *a, const WalkboxGraph *wbg) {
    if (!a->moving) return;

    if (!(a->moving & MOVE_NEW_LEG)) {
        if ((a->moving & MOVE_IN_LEG) && actor_walk_step(a))
            return;

        if (a->moving & MOVE_LAST_LEG) {
            a->moving = 0;
            actor_set_box(a, a->dest_box);
            // v4-6: startAnimActor(_standFrame) + turnToDirection(_walkdata.destdir)
            actor_start_anim((int)a->number, a->stand_frame);
            if (a->walk_dest_dir != -1 &&
                (int)a->target_facing != (int)a->walk_dest_dir) {
                a->target_facing = (uint16_t)a->walk_dest_dir;
                actor_set_facing((int)a->number, (int)a->walk_dest_dir);
            }
            return;
        }

        if (a->moving & MOVE_TURN) {
            // v4-6: setDirection(updateActorDirection(false)) or moving=0.
            int new_dir = update_actor_direction(a, false);
            if ((int)a->facing != new_dir)
                actor_set_facing((int)a->number, new_dir);
            else
                a->moving = 0;
            return;
        }

        actor_set_box(a, a->cur_box);
        a->moving &= MOVE_IN_LEG;
    }

    a->moving &= ~MOVE_NEW_LEG;
    int found_x = 0, found_y = 0;
    do {
        if (a->walkbox == INVALID_BOX) {
            actor_set_box(a, a->dest_box);
            a->cur_box = a->dest_box;
            break;
        }

        if (a->walkbox == a->dest_box) break;

        if (!wbg || !wbg->valid) break;

        uint8_t next_box = walkbox_next(wbg, a->walkbox, a->dest_box);
        if (next_box == INVALID_BOX) {
            a->dest_box = a->walkbox;
            a->moving |= MOVE_LAST_LEG;
            return;
        }

        a->cur_box = next_box;

        if (walkbox_find_path_towards(wbg, a->walkbox, next_box, a->dest_box,
                                      a->x, a->y, a->dest_x, a->dest_y,
                                      &found_x, &found_y))
            break;     // pass-through to dest

        if (actor_calc_movement_factor(a, found_x, found_y))
            return;

        actor_set_box(a, a->cur_box);
    } while (true);

    a->moving |= MOVE_LAST_LEG;
    actor_calc_movement_factor(a, a->dest_x, a->dest_y);
}

// Mirrors ScummEngine::Actor::setupActorScale (actor.cpp:451-474).
// For v4 the scale comes from the actor's current walkbox: if the
// box.scale 16-bit value has bit 0x8000 set it would index a SCAL
// slot (not loaded in v4), else it is the flat scale directly.
static void setup_actor_scale(Actor *a, const WalkboxGraph *wbg) {
    if (a->flags & ACTOR_FLAG_IGNORE_BOX) return;
    if (!wbg || !wbg->valid) return;
    if (a->walkbox >= wbg->num_boxes) return;
    uint16_t s = wbg->boxes[a->walkbox].scale;
    if (s & 0x8000) {
        // SCAL slot lookup — not modelled (v4 BOXM/SCAL absent in MI1).
        return;
    }
    if (s == 0) return;
    if (s > 0xFF) s = 0xFF;
    a->scalex = (uint8_t)s;
    a->scaley = (uint8_t)s;
}

// Mirrors Actor::animateCostume (actor.cpp:2877-2895). Once per frame
// (gated by _animSpeed), call increaseAnims to step every active limb.
static void tick_anim(Actor *a) {
    if (a->costume == 0) return;
    a->anim_progress++;
    if (a->anim_progress >= a->anim_speed) {
        a->anim_progress = 0;
        costume_increase_anims(a);
    }
    // Smooth turning toward target_facing, mirroring
    // Actor::updateActorDirection (actor.cpp:945-979). Each anim step
    // advances facing by at most kTurnStep degrees in the direction
    // that is the shortest arc to target. The 11° step matches
    // ScummVM's `(_facing - newDir) & 0xFF` 8-step rotation when the
    // costume's direction encoding is 8 cardinal directions
    // (45° per slot); we use 11° so the shortest arc resolves in
    // about 16 anim ticks for a 180° turn — perceptually matches
    // upstream's quarter-second rotate.
    if (a->facing != a->target_facing) {
        constexpr int kTurnStep = 11;
        int cur = (int)a->facing;
        int tgt = (int)a->target_facing;
        int delta = tgt - cur;
        while (delta > 180)  delta -= 360;
        while (delta < -180) delta += 360;
        if (delta >  kTurnStep) delta =  kTurnStep;
        if (delta < -kTurnStep) delta = -kTurnStep;
        cur += delta;
        while (cur < 0)    cur += 360;
        while (cur >= 360) cur -= 360;
        // Use the same setter so any side-effects (frame swap, etc.)
        // fire — but only for the per-tick incremental step so the
        // visible animation is continuous.
        a->facing = (uint16_t)cur;
    }
}

void actor_tick_all(const WalkboxGraph *wbg) {
    for (int i = 0; i < MAX_ACTORS; i++) {
        Actor &a = g_actors[i];
        if (!(a.flags & ACTOR_FLAG_VISIBLE)) continue;
        // MF_FROZEN — set by freezeActors. While frozen, walkActor and
        // animateCostume are skipped (actor.cpp:945+, 2877+).
        if (a.moving & MOVE_FROZEN) continue;
        // Refresh walkbox-derived scale before walking — needed because
        // the actor may have stepped into a new box on the previous
        // frame. Mirrors ScummEngine::Actor::setupActorScale called at
        // the top of walkActor (actor.cpp:949).
        if (wbg && wbg->valid && a.walkbox == INVALID_BOX) {
            a.walkbox = walkbox_at(wbg, a.x, a.y);
        }
        setup_actor_scale(&a, wbg);
        bool was_moving = (a.moving != 0);
        walk_actor(&a, wbg);
        tick_anim(&a);
        // Re-evaluate walkbox ONLY if the actor was walking. ScummVM
        // updates _walkbox in walkActor when a step crosses a box
        // boundary (actor.cpp:945+). For idle actors (e.g. floating
        // cloud sprites) _walkbox stays at its previous value — the
        // initActor default of 0 (actor.cpp:173) — which is what
        // getMaskFromBox(0) requires to route them onto z-plane 1.
        // Previously we re-evaluated unconditionally, so any actor
        // standing outside a walkbox got walkbox = INVALID_BOX (255)
        // → zbuf = 0 → no z-clip → clouds rendered in front of cliff.
        if (was_moving && wbg && wbg->valid) {
            uint8_t wb = walkbox_at(wbg, a.x, a.y);
            if (wb != INVALID_BOX) a.walkbox = wb;
        }
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
//
// Y-sort visible actors; for each, parse costume; for each non-empty limb,
// resolve the cel via the animation command stream; call
// costume_render_limb. Most v5 games drive limb cels through the
// per-anim-cmd table; without a complete state machine, we draw a single
// "default" cel per limb based on the actor's `frame` field. This makes
// the actor appear (a single static pose) — animation progresses over
// time once anim_speed > 0.

static int cmp_actor_y(const void *pa, const void *pb) {
    const Actor *a = *(const Actor *const *)pa;
    const Actor *b = *(const Actor *const *)pb;
    int ya = (int)a->y - (int)a->elevation;
    int yb = (int)b->y - (int)b->elevation;
    return ya - yb;
}

void actor_render_all(uint8_t *vscreen_main, int pitch,
                      const WalkboxGraph *wbg, int x_off) {
    if (!vscreen_main) return;

    // Build sorted pointer list
    const Actor *sorted[MAX_ACTORS];
    int nvis = 0;
    for (int i = 0; i < MAX_ACTORS; i++) {
        const Actor &a = g_actors[i];
        if (!(a.flags & ACTOR_FLAG_VISIBLE)) continue;
        if (a.costume == 0) continue;
        sorted[nvis++] = &a;
    }
    if (nvis == 0) return;

    qsort(sorted, nvis, sizeof(sorted[0]), cmp_actor_y);

    // Z-mask plumbing — mirrors ScummEngine::Actor::drawActorCostume
    // (actor.cpp:2577-2594). Each actor gets a `_zbuf` plane index; the
    // costume renderer then clips its pixels against that mask. _zbuf
    // selection priority:
    //   1. _forceClip   (set by op_actorOps SO_FORCE_CLIP / class flags)
    //   2. NEVER_ZCLIP  → no mask
    //   3. walkbox.mask & 0x03 (v4 GF_SMALL_HEADER, actor.cpp:2592)
    // Result is clamped to numZBuffer-1 (== engine_zmask_count()).
    int num_zplanes  = engine_zmask_count();
    int mask_pitch   = engine_zmask_pitch();

    for (int i = 0; i < nvis; i++) {
        const Actor &a = *sorted[i];
        Span cspan = resource_get_costume(a.costume);
        if (cspan.empty()) continue;
        CostumeData cd{};
        if (!costume_parse(cspan, &cd)) continue;

        // Mirrors scummvm Actor::drawActorCostume v3-v6 branch
        // (actor.cpp:2585-2594):
        //   if (_forceClip) zbuf = _forceClip;
        //   else if (isInClass(kObjectClassNeverClip)) zbuf = 0;
        //   else { zbuf = getMaskFromBox(walkbox); ... }
        // Note that NeverClip is a CLASS BIT checked at render — not
        // a flag mutated by classChanged. We previously set
        // ACTOR_FLAG_NEVER_ZCLIP from classChanged(20) which had two
        // problems: (a) it persisted across class clears, and (b) it
        // set the wrong semantics for actors that scummvm leaves at
        // zbuf=walkbox-mask (clouds in MI1 v4 floppy intro).
        int actor_id = (int)(&a - &g_actors[0]);
        int zbuf;
        if (a.force_clip) {
            zbuf = a.force_clip;
        } else if (engine_get_class(actor_id, 20)) {  // kObjectClassNeverClip
            zbuf = 0;
        } else if (wbg && wbg->valid && a.walkbox != INVALID_BOX) {
            zbuf = walkbox_mask_for_box(wbg, a.walkbox) & 0x03;
        } else {
            zbuf = 0;
        }
        if (zbuf > num_zplanes) zbuf = num_zplanes;
        const uint8_t *mask = (zbuf > 0) ? engine_zmask(zbuf) : nullptr;

        // _drawActorToRight — mirrors ClassicCostumeRenderer::setFacing
        // (costume.cpp:831-833): newDirToOldDir(_facing) != 0 ||
        // _loaded._mirror. dir 0 = west, others = right-half. So for a
        // facing of 0..70 / 290..359 (north / west) we draw left-to-right
        // unless the costume mirror flag forces it.
        int old_dir = costume_new_dir_to_old(a.facing);
        bool draw_to_right = (old_dir != 0) || cd.mirror;
        // Our flip_x flips the source columns; when draw_to_right is
        // false we want a mirrored draw.
        bool flip = !draw_to_right;
        if (a.flags & ACTOR_FLAG_FLIP_X) flip = !flip;

        // Per-actor xMove/yMove accumulators — reset for this draw call.
        // Mirrors ScummVM _xMove/_yMove (costume.cpp:618-644).
        int xmove = 0, ymove = 0;

        for (int l = 0; l < 16; l++) {
            // Mirrors ClassicCostumeRenderer::drawLimb (costume.cpp:594):
            // skip if curpos[l] == 0xFFFF or stopped & (1<<l).
            uint16_t cp = a.cost.curpos[l];
            if (cp == 0xFFFF) continue;
            if (a.cost.stopped_mask & (1 << l)) continue;

            // Frame-offsets must have an entry for this limb.
            const uint8_t *fo_entry = cd.frame_offsets + l * 2;
            if (fo_entry + 2 > cd.resource.data + cd.resource.size) break;
            uint16_t limb_off = read_le16(fo_entry);
            if (limb_off == 0) continue;

            // Resolve the cel index by reading the anim-cmd byte at the
            // current position (low 7 bits = cel; 0x7B = no-draw).
            int cel = 0;
            if (cd.anim_cmds) {
                size_t cmd_off = (size_t)((cd.anim_cmds - cd.baseptr) +
                                          (cp & 0x7FFF));
                if (cmd_off < cd.resource.size) {
                    uint8_t cmd = cd.baseptr[cmd_off];
                    if ((cmd & 0x7F) == 0x7B) continue;   // no-draw
                    cel = cmd & 0x7F;
                }
            }

            costume_render_limb(&cd, l, cel,
                                a.x - x_off, a.y - a.elevation,
                                a.scalex, a.scaley,
                                flip,
                                a.palette,
                                mask, mask_pitch, /*mask_x_off=*/x_off,
                                vscreen_main, pitch,
                                /*transparent_color=*/0,
                                &xmove, &ymove);
        }
    }
}

}  // namespace tsb
