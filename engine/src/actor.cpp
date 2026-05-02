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
#include "platform.h"

#include <string.h>
#include <stdlib.h>

namespace tsb {

static Actor g_actors[MAX_ACTORS];

// ---------------------------------------------------------------------------
// Initialization & simple setters
// ---------------------------------------------------------------------------

void actor_init_all() {
    memset(g_actors, 0, sizeof(g_actors));
    for (int i = 0; i < MAX_ACTORS; i++) {
        Actor &a = g_actors[i];
        a.number = (uint8_t)i;
        a.scalex = a.scaley = 0xFF;
        a.x = 0; a.y = 0;
        a.room = 0;
        a.speedx = 8; a.speedy = 2;          // ScummVM defaults
        a.facing = 180;
        a.target_facing = 180;
        a.walkbox = INVALID_BOX;
        a.cur_box = INVALID_BOX;
        a.dest_box = INVALID_BOX;
        a.init_frame = 1;
        a.walk_frame = 2;
        a.stand_frame = 3;
        a.talk_start_frame = 4;
        a.talk_stop_frame = 5;
        a.anim_speed = 0;
        a.anim_progress = 0;
        a.frame = a.init_frame;
        // identity palette
        for (int c = 0; c < 32; c++) a.palette[c] = (uint8_t)c;
        // Initialize costume anim slots to "empty"
        for (int l = 0; l < 16; l++) {
            a.cost.anim_type[l] = 0;
            a.cost.curpos[l]    = 0xFFFF;
            a.cost.start[l]     = 0xFFFF;
            a.cost.end[l]       = 0xFFFF;
            a.cost.frame[l]     = 0xFFFF;
        }
        a.cost.stopped_mask = 0xFFFF;       // all limbs stopped initially
    }
}

Actor *actor_get(int n) {
    if (n < 0 || n >= MAX_ACTORS) return nullptr;
    return &g_actors[n];
}

void actor_put_at(int n, int x, int y) {
    Actor *a = actor_get(n); if (!a) return;
    a->x = (int16_t)x; a->y = (int16_t)y;
    a->moving = 0;
    a->flags |= ACTOR_FLAG_VISIBLE;
}

void actor_put_in_room(int n, int room) {
    Actor *a = actor_get(n); if (!a) return;
    a->room = (uint8_t)room;
}

void actor_set_costume(int n, int cost) {
    Actor *a = actor_get(n); if (!a) return;
    a->costume = (uint16_t)cost;
    // Reset per-limb anim — keep existing positions but un-stop.
    a->cost.stopped_mask = 0xFFFF;
    for (int l = 0; l < 16; l++) {
        a->cost.curpos[l] = 0xFFFF;
        a->cost.frame[l]  = 0xFFFF;
    }
}

void actor_walk_to(int n, int x, int y) {
    Actor *a = actor_get(n); if (!a) return;
    a->dest_x = (int16_t)x; a->dest_y = (int16_t)y;
    a->moving |= MOVE_NEW_LEG;
}

void actor_animate(int n, int anim) {
    Actor *a = actor_get(n); if (!a) return;
    a->frame = (uint8_t)anim;
}

void actor_face_object(int n, int object) {
    (void)n; (void)object;
    // TODO: needs object-position lookup
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

// Advance the walk state for one actor.
static void tick_walk(Actor *a, const WalkboxGraph *wbg) {
    if (!(a->moving & (MOVE_NEW_LEG | MOVE_IN_LEG))) return;

    // Find current box if unknown.
    if (wbg && wbg->valid && a->cur_box == INVALID_BOX) {
        a->cur_box = walkbox_at(wbg, a->x, a->y);
    }

    if (a->moving & MOVE_NEW_LEG) {
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
            } else {
                // Plan next leg.
                a->moving |= MOVE_NEW_LEG;
            }
        }
    }
}

// Tick one actor's animation frame counter.
static void tick_anim(Actor *a) {
    if (a->anim_speed > 0) {
        if (++a->anim_progress >= a->anim_speed) {
            a->anim_progress = 0;
            // Advance per-limb curpos when we have a costume animation in
            // progress. For now we just bump a generic frame counter; the
            // costume command-stream walker is a stub.
            for (int l = 0; l < 16; l++) {
                if (a->cost.curpos[l] == 0xFFFF) continue;
                if (a->cost.curpos[l] >= a->cost.end[l]) {
                    a->cost.curpos[l] = a->cost.start[l];
                } else {
                    a->cost.curpos[l]++;
                }
            }
        }
    }
    // Sync facing toward target_facing (snap for now)
    a->facing = a->target_facing;
}

void actor_tick_all(const WalkboxGraph *wbg) {
    for (int i = 0; i < MAX_ACTORS; i++) {
        Actor &a = g_actors[i];
        if (!(a.flags & ACTOR_FLAG_VISIBLE)) continue;
        tick_walk(&a, wbg);
        tick_anim(&a);
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
                      const WalkboxGraph *wbg) {
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

    int num_strips = pitch / 8;
    (void)wbg;   // mask buffer not yet wired (z-planes not extracted)

    for (int i = 0; i < nvis; i++) {
        const Actor &a = *sorted[i];
        Span cspan = resource_get_costume(a.costume);
        if (cspan.empty()) continue;
        CostumeData cd{};
        if (!costume_parse(cspan, &cd)) continue;

        // Determine the cel index. In v5 the actor's `frame` field is an
        // animation index, which maps via animCmds[curpos[limb]] to a code.
        // Without the full anim machine, fall back to: for each limb, draw
        // the limb's first cel (cel index 0) if its frame_offsets entry is
        // non-zero. Tracks at minimum the limb=0 base sprite.
        bool flip = (a.flags & ACTOR_FLAG_FLIP_X) != 0;
        if (cd.mirror) {
            // costume can be drawn mirrored when actor faces left
            flip = flip ^ (a.facing >= 180 && a.facing < 360);
        }

        for (int l = 0; l < 16; l++) {
            // Skip empty limbs (frame_offsets entry is 0).
            const uint8_t *fo_entry = cd.frame_offsets + l * 2;
            if (fo_entry + 2 >
                cd.resource.data + cd.resource.size) break;
            uint16_t limb_off = read_le16(fo_entry);
            if (limb_off == 0) continue;

            // Determine cel: if the actor has set up curpos[l], use it
            // through the command stream; else default cel 0.
            int cel = 0;
            uint16_t cp = a.cost.curpos[l];
            if (cp != 0xFFFF && cd.anim_cmds) {
                size_t cmd_off = (size_t)((cd.anim_cmds - cd.baseptr) +
                                          (cp & 0x7FFF));
                if (cmd_off < cd.resource.size) {
                    uint8_t cmd = cd.baseptr[cmd_off];
                    cel = cmd & 0x7F;
                }
            }

            costume_render_limb(&cd, l, cel,
                                a.x, a.y - a.elevation,
                                a.scalex, a.scaley,
                                flip,
                                a.palette,
                                /*mask_buf=*/nullptr, num_strips,
                                vscreen_main, pitch,
                                /*transparent_color=*/0);
        }
    }
}

}  // namespace tsb
