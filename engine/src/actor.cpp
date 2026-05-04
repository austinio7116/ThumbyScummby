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

// Mirrors Actor::startAnimActor (actor.cpp:2692-2759). For v3+ if frame
// equals _initFrame, _cost.reset() is called first; then
// costumeDecodeData(frame, (uint)-1) refreshes every limb.
void actor_start_anim(int n, int frame) {
    Actor *a = actor_get(n); if (!a) return;
    // Resolve chore-redirect sentinels (actor.cpp:2714-2733):
    switch (frame) {
    case 0xFE: frame = a->init_frame; break;       // CHORE_REDIRECT_INIT
    case 0xFD: frame = a->walk_frame; break;       // CHORE_REDIRECT_WALK
    case 0xFC: frame = a->stand_frame; break;      // CHORE_REDIRECT_STAND
    case 0xFB: frame = a->talk_start_frame; break; // CHORE_REDIRECT_START_TALK
    case 0xFA: frame = a->talk_stop_frame; break;  // CHORE_REDIRECT_STOP_TALK
    default: break;
    }
    if (a->costume == 0) return;
    a->anim_progress = 0;
    if (frame == a->init_frame) {
        for (int l = 0; l < 16; l++) {
            a->cost.curpos[l] = 0xFFFF;
            a->cost.start[l]  = 0xFFFF;
            a->cost.end[l]    = 0xFFFF;
            a->cost.frame[l]  = 0xFFFF;
        }
        // Mirrors CostumeData::reset() (actor.h:81): no limbs are
        // stopped at the start of an init-frame anim — the decode
        // sets bits via 0x79 cmds, and clears them via 0x7A.
        a->cost.stopped_mask = 0;
    }
    costume_decode_data(a, frame, (unsigned)-1);
    a->frame = (uint8_t)frame;
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

void actor_put_at(int n, int x, int y) {
    Actor *a = actor_get(n); if (!a) return;
    a->x = (int16_t)x; a->y = (int16_t)y;
    a->moving = 0;
    a->flags |= ACTOR_FLAG_VISIBLE;
    // Mirror scummvm Actor::putActor: when the placed actor is the ego,
    // set _egoPositioned so o5_loadRoomWithEgo's post-ENCD fallback
    // knows the entry script handled positioning.
    if ((int)g_vm.globals[VAR_EGO] == n) {
        g_ego_positioned = true;
    }
}

void actor_put_in_room(int n, int room) {
    Actor *a = actor_get(n); if (!a) return;
    a->room = (uint8_t)room;
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

// Mirror of scummvm showActors (actor.cpp:2243-2250) + Actor::showActor
// (actor.cpp:2198+). For every actor whose `_room == _currentRoom`, set
// the visible flag. scummvm Actor::showActor only gates on
// `_currentRoom == 0` early-return and "already visible" — NOT on the
// costume field. Some actors (like NPCs whose costume gets set by an
// LSCR after the room transition) need to be flagged visible BEFORE
// their costume is set, otherwise the LSCR's actorOps can't find them
// running.
void actor_show_in_current_room(int current_room) {
    if (current_room == 0) return;
    for (int i = 0; i < MAX_ACTORS; i++) {
        Actor &a = g_actors[i];
        if ((int)a.room == current_room) {
            a.flags |= ACTOR_FLAG_VISIBLE;
        }
    }
}

void actor_set_costume(int n, int cost) {
    Actor *a = actor_get(n); if (!a) return;
    a->costume = (uint16_t)cost;
    // Mirrors ScummEngine::Actor::setActorCostume (actor.cpp:3690-3711)
    // for v4/v5 (non-GF_OLD_BUNDLE):
    //   _cost.reset();        — CostumeData::reset() at actor.h:78-89
    //                           sets stopped = 0 (NOT 0xFFFF), all
    //                           curpos/start/end/frame = 0xFFFF.
    //   for (i = 0; i < 32; i++) _palette[i] = 0xFF;
    //   _animProgress = 0; _needRedraw = true; _costumeNeedsInit = true;
    a->cost.stopped_mask = 0;
    for (int l = 0; l < 16; l++) {
        a->cost.curpos[l] = 0xFFFF;
        a->cost.start[l]  = 0xFFFF;
        a->cost.end[l]    = 0xFFFF;
        a->cost.frame[l]  = 0xFFFF;
    }
    for (int p = 0; p < 32; p++) a->palette[p] = 0xFF;
    a->anim_progress = 0;
    // Mirrors Actor::setActorCostume (actor.cpp:3672-3677): when the
    // actor is visible, immediately startAnimActor(_initFrame) so the
    // costume's idle pose plays. Without this, every limb stays at
    // 0xFFFF (cost.curpos == "empty") and the costume render path
    // skips them, producing invisible actors — symptom: MI1 cloud
    // actors (cost=111) flagged visible at room enter rendered
    // nothing until the boot script later issued an animateActor.
    if (a->flags & ACTOR_FLAG_VISIBLE) {
        actor_start_anim(n, a->init_frame);
    }
}

void actor_walk_to(int n, int x, int y) {
    Actor *a = actor_get(n); if (!a) return;
    a->dest_x = (int16_t)x; a->dest_y = (int16_t)y;
    a->moving |= MOVE_NEW_LEG;
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
// Walking
// ---------------------------------------------------------------------------
//
// We compute a per-frame 16.16 (xfrac, yfrac) increment from
// (next - cur), divided by max(|dx|, |dy|) and scaled by speed*scalex.
// Each frame we accumulate, integerize, advance position. When position
// reaches `next`, we either advance to the next box-gate or, on the final
// leg, clear MOVE_IN_LEG.

static int16_t clip16(int v) {
    if (v < -32767) return -32767;
    if (v >  32767) return  32767;
    return (int16_t)v;
}

// Compute (delta_x, delta_y, xfrac, yfrac) for the next leg toward (next_x,
// next_y) starting from current (a->x, a->y). Returns false if we're
// already there.
static bool start_leg(Actor *a, int next_x, int next_y) {
    int dx = next_x - a->x;
    int dy = next_y - a->y;
    if (dx == 0 && dy == 0) return false;

    a->next_x = (int16_t)next_x;
    a->next_y = (int16_t)next_y;

    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;

    // ScummVM v5 normalizes by deltaY (vertical-major) then re-checks vs
    // speedx; if the implied deltaX exceeds speedx, recomputes by deltaX.
    // We do the simpler formulation: compute step as a fixed-point fraction
    // such that the longer axis steps speed pixels per frame.
    int speedx = a->speedx ? a->speedx : 1;
    int speedy = a->speedy ? a->speedy : 1;

    int32_t dxf, dyf;
    if (abs_dy >= abs_dx) {
        // vertical-major
        dyf = (int32_t)speedy << 16;
        if (dy < 0) dyf = -dyf;
        // dxf = dyf * dx / dy
        if (dy != 0)
            dxf = (int32_t)((int64_t)dyf * dx / dy);
        else
            dxf = 0;
        // If implied |dxf>>16| > speedx, recompute by dxf
        int implied_dx = dxf < 0 ? -((int)(dxf >> 16)) : (int)(dxf >> 16);
        if (implied_dx > speedx) {
            dxf = (int32_t)speedx << 16;
            if (dx < 0) dxf = -dxf;
            if (dx != 0)
                dyf = (int32_t)((int64_t)dxf * dy / dx);
            else
                dyf = 0;
        }
    } else {
        dxf = (int32_t)speedx << 16;
        if (dx < 0) dxf = -dxf;
        if (dx != 0)
            dyf = (int32_t)((int64_t)dxf * dy / dx);
        else
            dyf = 0;
    }

    a->delta_x = dxf;
    a->delta_y = dyf;
    a->xfrac = 0;
    a->yfrac = 0;

    // Update target_facing — 4-direction
    if (abs_dy * 2 < abs_dx) a->target_facing = (dx > 0) ? 90 : 270;
    else                     a->target_facing = (dy > 0) ? 180 : 0;

    a->moving |= MOVE_IN_LEG;
    return true;
}

// One step: advance fractional accumulator, write integer pos. Returns
// true when the leg has been completed (we've reached a->next_x/y).
static bool step_leg(Actor *a) {
    int dx_target = (int)a->next_x - (int)a->x;
    int dy_target = (int)a->next_y - (int)a->y;
    if (dx_target == 0 && dy_target == 0) return true;

    // Scale by actor's scale: ScummVM uses (delta >> 8) * scale, which
    // simplifies to (delta * scale) / 256.
    int sx = a->scalex ? a->scalex : 0xFF;
    int sy = a->scaley ? a->scaley : 0xFF;

    int32_t step_x = (int32_t)(((int64_t)a->delta_x * sx) >> 8);
    int32_t step_y = (int32_t)(((int64_t)a->delta_y * sy) >> 8);

    int64_t tmp_x = ((int64_t)a->x << 16) + a->xfrac + step_x;
    int64_t tmp_y = ((int64_t)a->y << 16) + a->yfrac + step_y;

    a->xfrac = (int32_t)(tmp_x & 0xFFFF);
    a->yfrac = (int32_t)(tmp_y & 0xFFFF);
    a->x = clip16((int)(tmp_x >> 16));
    a->y = clip16((int)(tmp_y >> 16));

    // Clip to target if we overshot
    if ((dx_target > 0 && a->x > a->next_x) ||
        (dx_target < 0 && a->x < a->next_x)) a->x = a->next_x;
    if ((dy_target > 0 && a->y > a->next_y) ||
        (dy_target < 0 && a->y < a->next_y)) a->y = a->next_y;

    return (a->x == a->next_x && a->y == a->next_y);
}

// Mirrors Actor::startWalkAnim (actor.cpp:919-943). cmd 1 = start walk
// (startAnimActor(walkFrame)); cmd 3 = stop walk (startAnimActor(standFrame)).
static void start_walk_anim(Actor *a, int cmd, int angle) {
    if (a->walk_script) {
        // Mirrors Actor::startWalkAnim (actor.cpp:920-928): when an
        // actor has a custom walk script set via SO_WALK_ANIMATION,
        // dispatch it with (number, cmd, angle) and return — the
        // script is responsible for choosing frames itself.
        int32_t args[3] = { (int32_t)a->number, (int32_t)cmd, (int32_t)angle };
        vm_start_script(&g_vm, a->walk_script, args, 3, false, false);
        return;
    }
    if (cmd == 3 || cmd == 1) {
        actor_set_facing((int)a->number, angle == -1 ? a->facing : angle);
    }
    if (cmd == 1)      actor_start_anim((int)a->number, a->walk_frame);
    else if (cmd == 3) actor_start_anim((int)a->number, a->stand_frame);
}

// Advance the walk state for one actor.
static void tick_walk(Actor *a, const WalkboxGraph *wbg) {
    if (!(a->moving & (MOVE_NEW_LEG | MOVE_IN_LEG))) return;

    // Find current box if unknown.
    if (wbg && wbg->valid && a->cur_box == INVALID_BOX) {
        a->cur_box = walkbox_at(wbg, a->x, a->y);
    }

    if (a->moving & MOVE_NEW_LEG) {
        bool was_idle = !(a->moving & MOVE_IN_LEG);
        a->moving &= ~MOVE_NEW_LEG;
        // Pick next waypoint.
        int target_x = a->dest_x;
        int target_y = a->dest_y;

        if (wbg && wbg->valid) {
            // Find dest box; if not in any box, snap to closest box.
            uint8_t db = walkbox_at(wbg, target_x, target_y);
            if (db == INVALID_BOX && wbg->num_boxes > 0) {
                // Pick box whose closest point is nearest the destination.
                int best_d = 0x7FFFFFFF;
                int best_bx = 0;
                int best_x = target_x, best_y = target_y;
                for (int i = 0; i < wbg->num_boxes; i++) {
                    if (wbg->boxes[i].flags & BOX_FLAG_INVISIBLE) continue;
                    int cx, cy;
                    walkbox_closest_pt(&wbg->boxes[i], target_x, target_y, &cx, &cy);
                    int d = (cx - target_x) * (cx - target_x) +
                            (cy - target_y) * (cy - target_y);
                    if (d < best_d) {
                        best_d = d; best_bx = i;
                        best_x = cx; best_y = cy;
                    }
                }
                target_x = best_x; target_y = best_y;
                db = (uint8_t)best_bx;
            }
            a->dest_box = db;

            uint8_t next_box = walkbox_next(wbg, a->cur_box, a->dest_box);
            int nx = target_x, ny = target_y;
            if (next_box == INVALID_BOX || next_box == a->dest_box ||
                next_box == a->cur_box) {
                a->moving |= MOVE_LAST_LEG;
            } else {
                // Compute gate point: closest point on the boundary of
                // next_box to the destination.
                walkbox_closest_pt(&wbg->boxes[next_box], target_x, target_y,
                                   &nx, &ny);
            }
            if (!start_leg(a, nx, ny)) {
                a->moving = 0;
            }
        } else {
            // No walkbox graph — walk straight to destination.
            a->moving |= MOVE_LAST_LEG;
            if (!start_leg(a, target_x, target_y)) {
                a->moving = 0;
            }
        }
        // Trigger walk-frame on idle->moving transition. Mirrors
        // ScummEngine::Actor::walkActor which calls startWalkAnim(1)
        // when MF_NEW_LEG initiates motion.
        if (was_idle && (a->moving & MOVE_IN_LEG)) {
            start_walk_anim(a, 1, a->target_facing);
        }
        return;
    }

    if (a->moving & MOVE_IN_LEG) {
        bool reached = step_leg(a);
        if (reached) {
            a->moving &= ~MOVE_IN_LEG;
            // Update current box.
            if (wbg && wbg->valid)
                a->cur_box = walkbox_at(wbg, a->x, a->y);
            if (a->moving & MOVE_LAST_LEG) {
                a->moving = 0;
                // Stop-walk anim. Mirrors actor.cpp:957-963 — when
                // MF_LAST_LEG completes, startAnimActor(_standFrame)
                // and turnToDirection(_walkdata.destdir).
                start_walk_anim(a, 3, a->target_facing);
            } else {
                // Plan next leg.
                a->moving |= MOVE_NEW_LEG;
            }
        }
    }
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
        tick_walk(&a, wbg);
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
