// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — actor free-function compatibility layer (impl).
// Each function is a 1-3 line forwarder to the transcribed Actor /
// ScummEngine method.  See actor_compat.h for rationale.

#include "actor_compat.h"
#include "scummvm_compat.h"
#include "actor.h"

namespace tsb {

Actor *actor_get(int n) {
    if (n < 0 || n >= ScummEngine::kMaxActors) return nullptr;
    return g_scumm->_actors[n];
}

void actor_init_all() {
    for (int i = 0; i < ScummEngine::kMaxActors; i++) {
        if (g_scumm->_actors[i]) g_scumm->_actors[i]->initActor(-1);
    }
}

void actor_init_one(int n, int mode) {
    Actor *a = actor_get(n);
    if (a) a->initActor(mode);
}

void actor_set_costume(int n, int c) {
    Actor *a = actor_get(n);
    if (a) a->setActorCostume(c);
}

void actor_walk_to(int n, int x, int y) {
    Actor *a = actor_get(n);
    if (a) a->startWalkActor(x, y, -1);
}

void actor_start_walk(int n, int x, int y, int dir) {
    Actor *a = actor_get(n);
    if (a) a->startWalkActor(x, y, dir);
}

void actor_animate(int n, int anim) {
    Actor *a = actor_get(n);
    if (a) a->animateActor(anim);
}

void actor_animate_chore(int n, int anim) { actor_animate(n, anim); }

void actor_face_object(int n, int object) {
    Actor *a = actor_get(n);
    if (a) a->faceToObject(object);
}

void actor_set_facing(int n, int direction) {
    Actor *a = actor_get(n);
    if (a) a->setDirection(direction);
}

void actor_start_anim(int n, int frame) {
    Actor *a = actor_get(n);
    if (a) a->startAnimActor(frame);
}

void actor_class_changed(int n, int cls, bool set) {
    Actor *a = actor_get(n);
    if (a) a->classChanged(cls, set);
}

void actor_put_actor(int n, int x, int y, int new_room) {
    Actor *a = actor_get(n);
    if (a) a->putActor(x, y, new_room);
}

void actor_put_at(int n, int x, int y) {
    Actor *a = actor_get(n);
    if (a) a->putActor(x, y);
}

void actor_put_in_room(int n, int room) {
    Actor *a = actor_get(n);
    if (a) a->putActor(room);
}

void actor_hide(int n) {
    Actor *a = actor_get(n);
    if (a) a->hideActor();
}

void actor_hide_all() {
    for (int i = 0; i < ScummEngine::kMaxActors; i++) {
        Actor *a = actor_get(i);
        if (a) a->hideActor();
    }
}

void actor_show_in_current_room(int current_room) {
    // Mirrors ScummEngine::showActors — but transcribed showActors lives
    // in the #if 0'd tail of actor.cpp until step 4, so do it inline:
    // for each actor whose _room matches, showActor().
    if (current_room == 0) return;
    for (int i = 0; i < ScummEngine::kMaxActors; i++) {
        Actor *a = actor_get(i);
        if (a && a->_room == (byte)current_room) {
            a->showActor();
        }
    }
}

void actor_adjust_pos(Actor *a) {
    if (a) a->adjustActorPos();
}

bool actor_ego_positioned_get() { return g_scumm->_egoPositioned; }
void actor_ego_positioned_set(bool v) { g_scumm->_egoPositioned = v; }

// ---- Per-frame hooks --------------------------------------------------
// Transcribed ScummEngine::walkActors lives in actor.cpp.  We call it
// here, then drive animateCostume on each visible actor (transcribed
// Actor::animateCostume).
void actor_tick_all(const WalkboxGraph * /*wbg*/) {
    g_scumm->walkActors();
    for (int i = 0; i < ScummEngine::kMaxActors; i++) {
        Actor *a = actor_get(i);
        if (a && a->_visible) a->animateCostume();
    }
}

// Legacy renderer.  Adapted from src_old/actor.cpp:893+ — same logic,
// new scummvm Actor field names.  Replaces transcribed drawActorCostume
// until costume.cpp + gfx.cpp transcriptions land at steps 4/12.
}  // namespace tsb (close briefly to import legacy headers)

#include "costume.h"
#include "walkbox.h"
#include "resource.h"
#include "engine.h"
#include "object.h"        // for engine_get_class
#include <stdlib.h>

namespace tsb {

static int cmp_actor_y(const void *pa, const void *pb) {
    const Actor *a = *(const Actor *const *)pa;
    const Actor *b = *(const Actor *const *)pb;
    int ya = (int)a->_pos.y - (int)a->_elevation;
    int yb = (int)b->_pos.y - (int)b->_elevation;
    return ya - yb;
}

void actor_render_all(uint8_t *vscreen_main, int pitch,
                      const WalkboxGraph *wbg, int x_off) {
    if (!vscreen_main) return;

    const Actor *sorted[ScummEngine::kMaxActors];
    int nvis = 0;
    for (int i = 0; i < ScummEngine::kMaxActors; i++) {
        Actor *a = g_scumm->_actors[i];
        if (!a || !a->_visible || a->_costume == 0) continue;
        sorted[nvis++] = a;
    }
    if (nvis == 0) return;

    qsort(sorted, nvis, sizeof(sorted[0]), cmp_actor_y);

    int num_zplanes = engine_zmask_count();
    int mask_pitch  = engine_zmask_pitch();

    for (int i = 0; i < nvis; i++) {
        const Actor &a = *sorted[i];
        Span cspan = resource_get_costume(a._costume);
        if (cspan.empty()) continue;
        ParsedCostume cd{};
        if (!costume_parse(cspan, &cd)) continue;

        int actor_id = -1;
        for (int k = 0; k < ScummEngine::kMaxActors; k++)
            if (g_scumm->_actors[k] == &a) { actor_id = k; break; }

        int zbuf;
        if (a._forceClip) {
            zbuf = a._forceClip;
        } else if (actor_id >= 0 && engine_get_class(actor_id, 20)) {
            zbuf = 0;
        } else if (wbg && wbg->valid && a._walkbox != Actor::kInvalidBox) {
            zbuf = walkbox_mask_for_box(wbg, a._walkbox) & 0x03;
        } else {
            zbuf = 0;
        }
        if (zbuf > num_zplanes) zbuf = num_zplanes;
        const uint8_t *mask = (zbuf > 0) ? engine_zmask(zbuf) : nullptr;

        int old_dir = costume_new_dir_to_old((int)a._facing);
        bool draw_to_right = (old_dir != 0) || cd.mirror;
        bool flip = !draw_to_right;
        if (a._flip) flip = !flip;

        // scummvm Actor::_palette is uint16_t[256]; legacy renderer wants
        // uint8_t[32].  Copy the low byte.
        uint8_t pal8[32];
        for (int p = 0; p < 32; p++) pal8[p] = (uint8_t)a._palette[p];

        int xmove = 0, ymove = 0;
        for (int l = 0; l < 16; l++) {
            uint16_t cp = a._cost.curpos[l];
            if (cp == 0xFFFF) continue;
            if (a._cost.stopped & (1 << l)) continue;

            const uint8_t *fo_entry = cd.frame_offsets + l * 2;
            if (fo_entry + 2 > cd.resource.data + cd.resource.size) break;
            uint16_t limb_off = read_le16(fo_entry);
            if (limb_off == 0) continue;

            int cel = 0;
            if (cd.anim_cmds) {
                size_t cmd_off = (size_t)((cd.anim_cmds - cd.baseptr) +
                                          (cp & 0x7FFF));
                if (cmd_off < cd.resource.size) {
                    uint8_t cmd = cd.baseptr[cmd_off];
                    if ((cmd & 0x7F) == 0x7B) continue;
                    cel = cmd & 0x7F;
                }
            }

            costume_render_limb(&cd, l, cel,
                                a._pos.x - x_off, a._pos.y - a._elevation,
                                a._scalex, a._scaley,
                                flip,
                                pal8,
                                mask, mask_pitch, /*mask_x_off=*/x_off,
                                vscreen_main, pitch,
                                /*transparent_color=*/0,
                                &xmove, &ymove);
        }
    }
}

}  // namespace tsb
