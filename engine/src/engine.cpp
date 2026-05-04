// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — engine main loop. Skeleton only for Phase 1.

#include "engine.h"
#include "chunk.h"
#include "small_chunk.h"
#include "master_index.h"
#include "room.h"
#include "vm.h"
#include "resource.h"
#include "object.h"
#include "actor.h"
#include "walkbox.h"
#include "smap.h"
#include "opl2.h"
#include "adlib.h"
#include "imuse.h"
#include "audio_mix.h"
#include "text.h"
#include "charset.h"

#include <cmath>       // std::modf for frame-pacing fractional carry

#ifndef THUMBY_DEVICE
#include <stdio.h>
#include <stdlib.h>    // getenv, atoi
#endif
#include <string.h>

namespace tsb {

// Static state (no heap allocation). Sized for the device target budget.
//
// Two-tier screen buffer (mirrors ScummVM's `_virtscr[kMainVirtScreen]`
// and the visible window in `_compositeBuf` -- gfx.cpp:630-705):
//   vscreen_room = ROOM_BUFFER_W × 200 — full room-width composite of the
//                  background + objects. Populated once on room load.
//   vscreen_main = 320 × 200 — visible viewport, computed per frame by
//                  blitting strips [screenStartStrip..+39] from vscreen_room
//                  and then drawing actors on top in viewport-relative coords.
//
// Camera state (mirrors `camera._cur.x` / `_screenStartStrip` / `_screenEndStrip`
// in camera.cpp). VAR_CAMERA_MIN_X / VAR_CAMERA_MAX_X are set on room load
// to (screenWidth/2, roomWidth - screenWidth/2) per room.cpp:202-205.
struct Camera {
    int      cur_x;        // mirrors camera._cur.x (room x in pixels)
    int      dest_x;       // mirrors camera._dest.x — where panCameraTo wants
    int      last_x;
    uint8_t  mode;         // 1 = normal, 2 = follow_actor, 3 = panning
    uint8_t  movingToActor;
    int      follows;      // actor index when in follow mode
    int      leftTrigger;  // mirrors camera._leftTrigger (in strips, gfx.h:143)
    int      rightTrigger; // mirrors camera._rightTrigger
    int      screenStartStrip;
    int      screenEndStrip;
};
constexpr uint8_t kNormalCameraMode      = 1;
constexpr uint8_t kFollowActorCameraMode = 2;
constexpr uint8_t kPanningCameraMode     = 3;

struct EngineState {
    uint8_t  vscreen_room[ROOM_BUFFER_W * VIRTUAL_SCREEN_H];
    uint8_t  vscreen_main[VIRTUAL_SCREEN_W * VIRTUAL_SCREEN_H];
    // Mirrors ScummVM kTextVirtScreen — overlaid on top of vscreen_main
    // after the room+actor composite. Pixel value 0 = transparent
    // (passes the underlying main pixel). Cleared on stopTalk so the
    // displayed text doesn't bleed across talks. Audit H88.
    uint8_t  vscreen_text[VIRTUAL_SCREEN_W * VIRTUAL_SCREEN_H];

    // Z-plane mask for actor clipping. Decoded on demand into a single
    // room-wide buffer; `z_mask_cached_plane` tracks which plane (1..N)
    // currently lives in the buffer, or 0 when invalid (cache miss).
    // We hold the room+OBIM payload pointers in flash and re-decode the
    // requested plane when a different plane is asked for. Mirrors the
    // spirit of ScummVM's per-strip just-in-time decode at gfx.cpp::
    // decodeMask, batched here at plane granularity since our model
    // re-paints the whole vscreen_room on room load / drawObject.
    //
    // Sized for one plane only (was MAX_ZPLANES × MASK_BUF_SIZE = ~100KB
    // BSS) so the firmware fits the 520KB RP2350 budget.
    uint8_t  z_mask[MASK_BUF_SIZE];
    int      z_mask_cached_plane;     // 1..N when valid, 0 when invalid
    int      num_zplanes;

    Camera   camera;

    // Active palette: 256 RGB triplets, scaled to 0..255.
    uint8_t  palette[256 * 3];

    // _shadowPalette — 256-entry index remap. Mirrors ScummEngine::
    // _shadowPalette (palette.cpp:431-435 init, palette.cpp:741-768
    // cyclePalette). Initialised to identity (i -> i) on game start;
    // setupShadowPalette / cyclePalette mutate it. Applied during the
    // room->screen blit: each pixel `p` is rendered as `palette[shadow[p]]`.
    uint8_t  shadow_palette[256];
    bool     shadow_dirty;       // any non-identity entry?

    // Master directory parsed from 000.LFL
    MasterIndex master;

    // Currently loaded room
    Room     room;
    int      current_room_id;
    bool     room_loaded;

    // Walkbox graph for the current room
    WalkboxGraph walkboxes;

    // Display state
    platform::ScaleMode scale_mode;
    int      crop_x, crop_y;

    // Frame counter
    uint32_t frame;

    bool     initialized;
    bool     quitting;
    bool     boot_started;
    bool     skip_boot_script;
    int      unimpl_log_count;     // throttle "unimpl" spam
};

static EngineState g{};

// Global object state / owner tables — mirrors ScummVM _objectStateTable /
// _objectOwnerTable. Indexed by global obj_nr; zero on game start.
static uint8_t g_object_state[NUM_GLOBAL_OBJECTS] = {};
static uint8_t g_object_owner[NUM_GLOBAL_OBJECTS] = {};

// Per-game var-table indices that depend on game version. 0xFF is the
// "var not defined for this game" sentinel matching ScummVM's pattern
// (scumm.h:1947). v4 doesn't define VAR_MAIN_SCRIPT so it stays 0xFF.
// When v5+ ports come, set this in setup-vars to the right index (e.g.
// 127 for the games per vars.cpp:359).
static int g_var_main_script = 0xFF;

// Per-object class bitfield — mirrors ScummEngine::_classData. 32 class
// bits per object packed into a uint32_t.
static uint32_t g_object_classes[NUM_GLOBAL_OBJECTS] = {};

// Inventory: ScummVM's _inventory[] is sized by VAR_INVENTORY_SLOT_COUNT
// (80 in MI1). Slots start at index 1 (slot 0 is unused, matching
// ScummVM convention). When an obj is "carried", its global owner field
// is set to actor_num and an entry is placed in _inventory.
constexpr int INVENTORY_MAX = 80;
static uint16_t g_inventory[INVENTORY_MAX + 1];   // indexed 1..INVENTORY_MAX

// String resource pool (rtString in ScummVM). Used by op_stringOps.
struct StringSlot { uint8_t data[STRING_SLOT_SIZE]; int size; bool present; };
static StringSlot g_strings[STRING_SLOT_COUNT];

void engine_string_load(int slot, const uint8_t *src) {
    if (slot < 0 || slot >= STRING_SLOT_COUNT) return;
    int n = 0;
    if (src) {
        while (src[n] != 0 && n < STRING_SLOT_SIZE - 1) n++;
        memcpy(g_strings[slot].data, src, (size_t)n);
    }
    g_strings[slot].data[n] = 0;
    g_strings[slot].size = n + 1;
    g_strings[slot].present = true;
}

void engine_string_create_empty(int slot, int size) {
    if (slot < 0 || slot >= STRING_SLOT_COUNT) return;
    if (size < 0) size = 0;
    if (size > STRING_SLOT_SIZE) size = STRING_SLOT_SIZE;
    memset(g_strings[slot].data, 0, (size_t)size);
    g_strings[slot].size = size;
    g_strings[slot].present = (size > 0);
}

uint8_t *engine_string_data(int slot, int *out_size) {
    if (slot < 0 || slot >= STRING_SLOT_COUNT) return nullptr;
    if (!g_strings[slot].present) return nullptr;
    if (out_size) *out_size = g_strings[slot].size;
    return g_strings[slot].data;
}

void engine_string_set_char(int slot, int idx, uint8_t c) {
    if (slot < 0 || slot >= STRING_SLOT_COUNT) return;
    if (!g_strings[slot].present) return;
    if (idx < 0 || idx >= g_strings[slot].size) return;
    g_strings[slot].data[idx] = c;
}

uint8_t engine_string_get_char(int slot, int idx) {
    if (slot < 0 || slot >= STRING_SLOT_COUNT) return 0;
    if (!g_strings[slot].present) return 0;
    if (idx < 0 || idx >= g_strings[slot].size) return 0;
    return g_strings[slot].data[idx];
}

void engine_string_copy(int dst_slot, int src_slot) {
    if (dst_slot < 0 || dst_slot >= STRING_SLOT_COUNT) return;
    if (src_slot < 0 || src_slot >= STRING_SLOT_COUNT) return;
    g_strings[dst_slot] = g_strings[src_slot];
}

// Object-name pool. ScummVM uses rtObjectName resource type with one
// entry per global object id; storage is variable-length so we use a
// small fixed pool of slots and an index map. 32 bytes per name covers
// every MI1 object name.
constexpr int OBJ_NAME_POOL = 256;
constexpr int OBJ_NAME_LEN  = 64;
static int     g_obj_name_count = 0;
static int16_t g_obj_name_index[NUM_GLOBAL_OBJECTS];     // -1 if no name
static uint8_t g_obj_name_pool[OBJ_NAME_POOL][OBJ_NAME_LEN];

uint8_t engine_get_object_state(int obj_id) {
    if (obj_id < 0 || obj_id >= NUM_GLOBAL_OBJECTS) return 0;
    return g_object_state[obj_id];
}
void engine_put_object_state(int obj_id, uint8_t state) {
    if (obj_id < 0 || obj_id >= NUM_GLOBAL_OBJECTS) return;
    g_object_state[obj_id] = state;
}
uint8_t engine_get_object_owner(int obj_id) {
    if (obj_id < 0 || obj_id >= NUM_GLOBAL_OBJECTS) return 0;
    return g_object_owner[obj_id];
}
void engine_put_object_owner(int obj_id, uint8_t owner) {
    if (obj_id < 0 || obj_id >= NUM_GLOBAL_OBJECTS) return;
    g_object_owner[obj_id] = owner;
}

// Mirrors ScummEngine::getClass / putClass (object.cpp:225-294). For
// SMALL_HEADER (v4) games, four "new" class IDs are remapped to the
// "old" range:
//   kObjectClassUntouchable(32) -> 24
//   kObjectClassPlayer(31)      -> 23
//   kObjectClassXFlip(30)       -> 19
//   kObjectClassYFlip(29)       -> 18
// Negative `cls` in script form encodes a clear; the polarity comes
// from the high bit (& 0x80) — putClass(_, cls, (cls & 0x80) != 0).
static int remap_class_v4(int cls) {
    cls &= 0x7F;
    switch (cls) {
    case 32: return 24;     // Untouchable
    case 31: return 23;     // Player
    case 30: return 19;     // XFlip
    case 29: return 18;     // YFlip
    default: return cls;
    }
}

bool engine_get_class(int obj_id, int cls) {
    if (obj_id < 0 || obj_id >= NUM_GLOBAL_OBJECTS) return false;
    int c = remap_class_v4(cls);
    if (c < 1 || c > 32) return false;
    return (g_object_classes[obj_id] & (1u << (c - 1))) != 0;
}
void engine_put_class(int obj_id, int cls, bool set) {
    if (obj_id < 0 || obj_id >= NUM_GLOBAL_OBJECTS) return;
    int c = remap_class_v4(cls);
    if (c < 1 || c > 32) return;
    if (set) g_object_classes[obj_id] |=  (1u << (c - 1));
    else     g_object_classes[obj_id] &= ~(1u << (c - 1));
    // ScummVM forwards class bits 18..30 onto actors when obj < numActors —
    // see object.cpp:291-293 + Actor::classChanged.
    if (obj_id >= 1 && obj_id < MAX_ACTORS) {
        actor_class_changed(obj_id, c, set);
    }
}
void engine_clear_class_data(int obj_id) {
    if (obj_id < 0 || obj_id >= NUM_GLOBAL_OBJECTS) return;
    g_object_classes[obj_id] = 0;
}

// Inventory ownership. ScummVM's addObjectToInventory walks the
// _inventory pool to find a free slot and writes obj_id there; remove
// shifts entries down. We mirror that directly.
int engine_inventory_count(int actor_num) {
    int n = 0;
    for (int i = 1; i <= INVENTORY_MAX; i++) {
        if (g_inventory[i] == 0) continue;
        if (engine_get_object_owner(g_inventory[i]) == (uint8_t)actor_num)
            n++;
    }
    return n;
}
int engine_find_inventory(int actor_num, int idx) {
    int seen = 0;
    for (int i = 1; i <= INVENTORY_MAX; i++) {
        if (g_inventory[i] == 0) continue;
        if (engine_get_object_owner(g_inventory[i]) == (uint8_t)actor_num) {
            if (++seen == idx) return g_inventory[i];
        }
    }
    return 0;
}
void engine_add_object_to_inventory(int obj_id, int actor_num) {
    // already there?
    for (int i = 1; i <= INVENTORY_MAX; i++) if (g_inventory[i] == obj_id) {
        engine_put_object_owner(obj_id, (uint8_t)actor_num);
        return;
    }
    for (int i = 1; i <= INVENTORY_MAX; i++) {
        if (g_inventory[i] == 0) {
            g_inventory[i] = (uint16_t)obj_id;
            engine_put_object_owner(obj_id, (uint8_t)actor_num);
            return;
        }
    }
}
void engine_remove_object_from_inventory(int obj_id) {
    for (int i = 1; i <= INVENTORY_MAX; i++) {
        if (g_inventory[i] == obj_id) {
            g_inventory[i] = 0;
            return;
        }
    }
}

// Refresh each loaded ObjectData's cached `state` from the global table.
// Mirrors ScummEngine::updateObjectStates (object.cpp:1165). Called after
// room_load and after any setState/setOwner that might affect what shows.
static void refresh_object_states(ObjectTable *t) {
    for (int i = 1; i <= t->num_objects; i++) {
        ObjectData *o = &t->objects[i];
        if (o->obj_id > 0)
            o->state = g_object_state[o->obj_id];
    }
}

// Forward — defined later in this file as the engine's only ObjectTable.
static ObjectTable g_object_table{};
ObjectTable *get_object_table() { return &g_object_table; }

// Mirrors ScummEngine::getObjectXYPos (object.cpp:506-509) for
// version 3-4: returns od->walk_x / od->walk_y / actordir-derived
// direction (oldDirToNewDir(actordir & 3)).
static int old_dir_to_new_dir(int dir) {
    static const int new_dir_table[4] = { 270, 90, 180, 0 };
    return new_dir_table[dir & 3];
}

bool engine_object_walk_pos(int obj_id, int *out_x, int *out_y, int *out_dir) {
    ObjectData *o = object_get_by_id(&g_object_table, obj_id);
    if (!o) return false;
    if (out_x)   *out_x = o->walk_x;
    if (out_y)   *out_y = o->walk_y;
    if (out_dir) *out_dir = old_dir_to_new_dir(o->actor_dir);
    return true;
}

// Mirrors ScummEngine::findObject (object.cpp:557-595) — v3-5 path
// without HE polygons. Honours kObjectClassUntouchable (v4 class 24).
int engine_find_object_at(int x, int y) {
    constexpr int mask = 0x0F;
    for (int i = 1; i <= g_object_table.num_objects; i++) {
        ObjectData *o = &g_object_table.objects[i];
        if (o->obj_id < 1) continue;
        if (engine_get_class(o->obj_id, 32 /*kObjectClassUntouchable*/))
            continue;
        // Walk parent chain — must reach parent==0 with all (state&mask)
        // matches the child's parentstate, exactly like upstream's loop.
        int b = i;
        while (true) {
            uint8_t a = g_object_table.objects[b].parentstate;
            int p = g_object_table.objects[b].parent;
            b = p;
            if (b == 0) {
                int x0 = o->x_strip * 8, y0 = o->y * 8;
                int x1 = x0 + o->w_strip * 8;
                int y1 = y0 + o->h * 8;
                if (x0 <= x && x < x1 && y0 <= y && y < y1) return o->obj_id;
                break;
            }
            if ((g_object_table.objects[b].state & mask) != a) break;
        }
    }
    return 0;
}

// Mirrors ScummEngine::getDist (object.cpp:516-520) — Chebyshev metric.
int engine_world_dist(int x1, int y1, int x2, int y2) {
    int a = y1 > y2 ? (y1 - y2) : (y2 - y1);
    int b = x1 > x2 ? (x1 - x2) : (x2 - x1);
    return a > b ? a : b;
}

// Mirrors o5_walkActorToObject (script_v5.cpp:2120-2158): if the object
// has a walk-pos, walk the actor to (walk_x, walk_y).
void engine_walk_actor_to_object(int actor_num, int obj_id) {
    int x, y, dir;
    if (!engine_object_walk_pos(obj_id, &x, &y, &dir)) return;
    actor_walk_to(actor_num, x, y);
    Actor *a = actor_get(actor_num);
    if (a) a->target_facing = (uint16_t)dir;
}
void engine_put_actor_at_object(int actor_num, int obj_id) {
    int x, y, dir;
    if (!engine_object_walk_pos(obj_id, &x, &y, &dir)) return;
    actor_put_at(actor_num, x, y);
    Actor *a = actor_get(actor_num);
    if (a) { a->facing = (uint16_t)dir; a->target_facing = (uint16_t)dir; }
}

// Mirrors ScummEngine::drawBox (gfx.cpp). Renders a filled rectangle
// at room coords into the room-wide composite buffer. The next viewport
// blit picks it up.
void engine_draw_box(int x1, int y1, int x2, int y2, int color) {
    if (!g.room_loaded) return;
    if (color < 0) color = 0;
    int rw = g.room.width;
    int rh = g.room.height;
    if (rw > ROOM_BUFFER_W) rw = ROOM_BUFFER_W;
    if (rh > VIRTUAL_SCREEN_H) rh = VIRTUAL_SCREEN_H;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > rw - 1) x2 = rw - 1;
    if (y2 > rh - 1) y2 = rh - 1;
    for (int y = y1; y <= y2; y++) {
        uint8_t *row = g.vscreen_room + (size_t)y * ROOM_BUFFER_W;
        for (int x = x1; x <= x2; x++) row[x] = (uint8_t)color;
    }
}

// Sentence stack. Mirrors ScummEngine::_sentence[] + _sentenceNum.
static SentenceEntry g_sentence[NUM_SENTENCE];
static int           g_sentence_num = 0;

void engine_sentence_push(int verb, int obj_a, int obj_b) {
    if (g_sentence_num >= NUM_SENTENCE) return;
    SentenceEntry &st = g_sentence[g_sentence_num++];
    st.verb         = (uint8_t)verb;
    st.object_a     = (uint16_t)obj_a;
    st.object_b     = (uint16_t)obj_b;
    st.preposition  = (obj_b != 0);
    st.freeze_count = 0;
}

void engine_sentence_tick() {
    int sentence_script = (int)g_vm.globals[VAR_SENTENCE_SCRIPT];
    if (sentence_script == 0) return;
    // Mirrors script.cpp:1178-1183: don't pop if the sentence script
    // is currently running and not frozen.
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        if (g_vm.slots[i].script_num == (uint16_t)sentence_script &&
            g_vm.slots[i].status != SS_DEAD &&
            g_vm.slots[i].freeze_count == 0) return;
    }
    if (g_sentence_num <= 0) return;
    if (g_sentence[g_sentence_num - 1].freeze_count) return;
    g_sentence_num--;
    SentenceEntry &st = g_sentence[g_sentence_num];
    // For v3+ (script.cpp:1195): if (preposition && objectB == objectA) return.
    if (st.preposition && st.object_b == st.object_a) return;
    int32_t args[3] = { st.verb, st.object_a, st.object_b };
    vm_start_script(&g_vm, sentence_script, args, 3, false, false);
}

// Mirrors ScummEngine::getVerbEntrypoint (script.cpp:167-258) for v4
// SMALL_HEADER. The verb-script directory in OBCD starts at chunk
// offset 19 (i.e. payload + 13 in our parser, which strips the 6-byte
// chunk header). Each entry is 3 bytes: verb-id, LE16 offset (relative
// to the OBCD chunk start). Terminator: a 0 verb-id.
int engine_get_verb_entrypoint(int obj_id, int verb,
                               Span *out_payload, uint32_t *out_offset) {
    ObjectData *od = object_get_by_id(&g_object_table, obj_id);
    if (!od) return 0;
    // OBCD payload begins after the 6-byte small_chunk header. ScummVM's
    // verbptr = objptr+19 with objptr at the chunk start. Our payload =
    // objptr+6, so the verb directory begins at payload offset 13.
    const uint8_t *p = od->obcd_payload.data;
    size_t sz = od->obcd_payload.size;
    if (sz < 14) return 0;
    const uint8_t *vptr = p + 13;
    while (vptr + 3 <= p + sz) {
        if (*vptr == 0) return 0;
        if (*vptr == (uint8_t)verb || *vptr == 0xFF) {
            uint16_t offs = read_le16(vptr + 1);
            // ScummVM v4 SMALL_HEADER returns offset directly (chunk-
            // start-relative); we want the bytecode payload to feed
            // into vm_start_room_script. The bytecode begins at
            // (chunk_start + offs). Chunk start is payload - 6.
            const uint8_t *bytecode = (p - 6) + offs;
            // Sanity bound: clamp size to room data
            if (out_payload) {
                size_t remaining = (size_t)((p + sz) - bytecode);
                out_payload->data = bytecode;
                out_payload->size = remaining;
            }
            if (out_offset) *out_offset = offs;     // chunk-start-relative
            return offs;
        }
        vptr += 3;
    }
    return 0;
}

// Mirrors ScummEngine::runObjectScript (script.cpp:130-150). Looks up
// the verb's entrypoint, then starts the script with where=WIO_FLOBJECT.
int engine_run_object_script(int obj_id, int verb,
                             bool freeze_resistant, bool recursive,
                             const int32_t *args, int n_args) {
    Span code{}; uint32_t offs = 0;
    int found = engine_get_verb_entrypoint(obj_id, verb, &code, &offs);
    if (!found || code.empty()) return -1;
    // We don't yet have a vm_start_object_script wrapper; reuse
    // vm_start_room_script with a pseudo-num that tags the obj/verb
    // pair (low 16 bits = obj, high bits = verb).
    int pseudo = obj_id | (verb << 16);
    (void)freeze_resistant; (void)recursive; (void)args; (void)n_args;
    return vm_start_room_script(&g_vm, code, pseudo, offs, WHERE_FLOBJ);
}

// Mirrors ScummEngine::setPalColor (palette.cpp). The palette source
// here uses unscaled 0..255 RGB matching the v4 PA chunk; ScummVM
// scales 6-bit DOS palette values, but our palette is already 8-bit.
void engine_set_pal_color(int d, int r, int gv, int b) {
    if (d < 0 || d >= 256) return;
    g.palette[d * 3 + 0] = (uint8_t)(r  & 0xFF);
    g.palette[d * 3 + 1] = (uint8_t)(gv & 0xFF);
    g.palette[d * 3 + 2] = (uint8_t)(b  & 0xFF);
}

// Mirrors ScummEngine::darkenPalette (palette.cpp). Scales each RGB
// component at indices [start..end] by (scale/0xFF), writing back to
// _currentPalette. ScummVM also clamps to a baseline copy (the
// _darkenPalette baseline) but for our v4 path the palette is kept
// fresh from the room load — no baseline needed.
void engine_darken_palette(int rs, int gs, int bs, int start, int end) {
    if (start < 0) start = 0;
    if (end > 255) end = 255;
    for (int i = start; i <= end; i++) {
        g.palette[i * 3 + 0] = (uint8_t)((g.palette[i*3+0] * rs) >> 8);
        g.palette[i * 3 + 1] = (uint8_t)((g.palette[i*3+1] * gs) >> 8);
        g.palette[i * 3 + 2] = (uint8_t)((g.palette[i*3+2] * bs) >> 8);
    }
}

void engine_set_object_name(int obj_id, const uint8_t *name, int len) {
    if (obj_id < 0 || obj_id >= NUM_GLOBAL_OBJECTS) return;
    if (!name) return;
    int idx = g_obj_name_index[obj_id];
    if (idx < 0) {
        if (g_obj_name_count >= OBJ_NAME_POOL) return;
        idx = g_obj_name_count++;
        g_obj_name_index[obj_id] = (int16_t)idx;
    }
    if (len > OBJ_NAME_LEN - 1) len = OBJ_NAME_LEN - 1;
    memcpy(g_obj_name_pool[idx], name, (size_t)len);
    g_obj_name_pool[idx][len] = 0;
}
const uint8_t *engine_get_object_name(int obj_id) {
    if (obj_id < 0 || obj_id >= NUM_GLOBAL_OBJECTS) return nullptr;
    int idx = g_obj_name_index[obj_id];
    if (idx < 0) return nullptr;
    return g_obj_name_pool[idx];
}

// Mirror ScummEngine::scummLoop_updateScummVars (scumm.cpp:2989-3008).
// VAR_TIMER counts ticks since last frame; VAR_TIMER_TOTAL accumulates;
// VAR_TMR_1/2/3 also accumulate. delta is in 60ths-of-a-second ticks
// (we'll feed it from frame pacing in engine_tick).
void engine_update_scumm_vars(int delta_ticks) {
    g_vm.globals[VAR_TIMER]       = delta_ticks;
    g_vm.globals[VAR_TIMER_TOTAL] += delta_ticks;
    g_vm.globals[VAR_TMR_1]       += delta_ticks;
    g_vm.globals[VAR_TMR_2]       += delta_ticks;
    g_vm.globals[VAR_TMR_3]       += delta_ticks;
}

// Expose master index to resource.cpp
MasterIndex *resource_get_master_index() { return &g.master; }

// Exposed to walkbox.cpp so o5_isActorInBox / o5_matrixOps can query the
// current room's graph without engine.cpp depending on walkbox internals.
WalkboxGraph *engine_active_walkbox_graph() {
    return g.walkboxes.valid ? &g.walkboxes : nullptr;
}

// Exposed to string.cpp — the text VirtScreen. Mirrors ScummVM
// kTextVirtScreen: a transparent overlay drawn on top of the main
// composite each frame. The sentinel for "no text here, fall through"
// is CHARSET_MASK_TRANSPARENCY (0xFD) — see scummvm gfx.h:289 and the
// composite blit in scummvm gfx.cpp:710 which skips bytes equal to that
// value. Using 0xFD (not 0) lets shadow glyph pixels written as palette
// index 0 (black) survive the composite, matching original behaviour.
uint8_t *engine_main_vscreen()       { return g.vscreen_text; }
int      engine_main_vscreen_pitch() { return VIRTUAL_SCREEN_W; }

// Clear the text overlay — called by stopTalk and at engine init.
void engine_clear_text_vscreen() {
    memset(g.vscreen_text, 0xFD, sizeof(g.vscreen_text));
}

Span     engine_room_excd_payload() { return g.room.excd_payload; }
uint32_t engine_room_excd_offset()  { return g.room.excd_offset; }
Span     engine_room_encd_payload() { return g.room.encd_payload; }
uint32_t engine_room_encd_offset()  { return g.room.encd_offset; }
int      engine_current_room_id()   { return g.current_room_id; }
uint8_t *engine_room_buffer()       { return g.vscreen_room; }
int      engine_room_width()        { return g.room_loaded ? g.room.width  : ROOM_BUFFER_W; }
int      engine_room_height()       { return g.room_loaded ? g.room.height : VIRTUAL_SCREEN_H; }

// Set by vm_opcodes_v4_init(); read by opcodes that branch on v4-vs-v5
// operand shape.
static bool g_v4_mode = false;
bool engine_is_v4()                 { return g_v4_mode; }
void engine_set_v4_mode(bool on)    { g_v4_mode = on; }

Span engine_local_script(int script_id, uint32_t *out_offset) {
    int idx = script_id - 200;
    if (idx < 0 || idx >= Room::MAX_LOCAL_SCRIPTS) return Span{nullptr, 0};
    if (out_offset) *out_offset = g.room.lscr_offset[idx];
    return g.room.lscr_payload[idx];
}

// ---------------------------------------------------------------------------
// Z-plane mask decode-on-demand. The room's BM-level ZP chain (in flash)
// plus every visible object's OBIM-level ZP chain combine into per-plane
// 1bpp masks; only the plane an actor currently needs is decoded into
// g.z_mask. Mirrors ScummVM Gdi::decodeMask (gfx.cpp:2565+) which itself
// decodes on demand per-strip — we batch at plane granularity because
// our model repaints the whole vscreen_room on room load / drawObject.
//
// Object-level z-planes overwrite (not OR onto) the room's mask at the
// object's strip range. Mirrors ScummVM's `decompressMaskImg` (overwrite)
// fallback — `decompressMaskImgOr` is only used with the dbAllowMaskOr
// flag, which v4 OBIM doesn't set.
// ---------------------------------------------------------------------------

// Walks a BM/OBIM chained-offset z-plane header and decodes ONLY the
// plane at `target_idx` (0-based) into `dst_buf`. Mirrors the same loop
// as the previous all-planes decoder but skips the planes we don't need.
static void decode_zplane_chain_one(Span bm_payload,
                                    int width, int height,
                                    int dst_strip_off,
                                    bool overwrite,
                                    int target_idx,
                                    uint8_t *dst_buf) {
    if (bm_payload.size < 4) return;
    if (target_idx < 0 || target_idx >= g.num_zplanes) return;
    const uint8_t *base = bm_payload.data;
    size_t base_size    = bm_payload.size;

    uint32_t off = read_le32(base);
    const uint8_t *plane = base;
    int idx = 0;
    bool decoded = false;
    while (off && idx <= target_idx) {
        plane += off;
        if (plane < base ||
            (size_t)(plane - base) + 2 > base_size) break;
        size_t plane_off  = (size_t)(plane - base);
        size_t plane_avail = base_size - plane_off;
        uint16_t next_off = read_le16(plane);
        size_t plane_len = next_off ? next_off : plane_avail;
        if (plane_len > plane_avail) plane_len = plane_avail;

        if (idx == target_idx) {
            smap_decode_zplane(Span{plane, plane_len},
                               width, height,
                               dst_buf, MASK_BUF_PITCH,
                               dst_strip_off,
                               /*or_mode=*/!overwrite);
            decoded = true;
            break;
        }
        idx++;
        off = next_off;
    }
    // If overwrite and the chain didn't supply our target plane, zero
    // the strips we cover. Mirrors the "missing plane = transparent"
    // semantic of the previous all-planes decoder.
    if (overwrite && !decoded) {
        int strips = width / 8;
        for (int s = 0; s < strips; s++) {
            int dst_strip = dst_strip_off + s;
            if (dst_strip < 0 || dst_strip >= MASK_BUF_PITCH) continue;
            uint8_t *col = dst_buf + dst_strip;
            for (int y = 0; y < height; y++) col[y * MASK_BUF_PITCH] = 0;
        }
    }
}

// Decode ZP plane `plane_idx` (1-based, matching engine_zmask's contract)
// into g.z_mask. Mirrors the all-planes decoder's two-pass shape:
//   1. Room-level BM chain across full room width (overwrite)
//   2. Visible objects' OBIM chains in reverse order (overwrite at strips)
static void decode_plane_into_cache(int plane_idx) {
    int target = plane_idx - 1;
    memset(g.z_mask, 0, MASK_BUF_SIZE);
    if (target < 0 || target >= g.num_zplanes) return;
    if (g.room.bm_smap_payload.empty()) return;

    int rh = g.room.height;
    if (rh > VIRTUAL_SCREEN_H) rh = VIRTUAL_SCREEN_H;

    // Room-level z-plane: overwrite across the full room width.
    decode_zplane_chain_one(g.room.bm_smap_payload,
                            g.room.width, rh,
                            /*dst_strip_off=*/0,
                            /*overwrite=*/true,
                            target,
                            g.z_mask);

    // Visible objects' z-planes: overwrite at the object's strip range.
    // Walk in the same reverse order as object_render_all so higher-
    // priority objects (later in OC sibling order) win at draws.
    for (int i = g_object_table.num_objects; i >= 1; i--) {
        const ObjectData &o = g_object_table.objects[i];
        if (o.obj_id == 0 || !(o.state & 0x0F)) continue;
        if (o.obim_payload.size < 4) continue;
        if (o.w_strip == 0 || o.h == 0) continue;

        int oh = o.h * 8;
        int ow = o.w_strip * 8;
        if (oh > rh) oh = rh;
        // object_render_all subs payload by 2 (skip obj_id) before passing
        // to smap_decode_bm; the BM-style chained-offset header is at +2
        // within the OBIM payload as well.
        Span obim_bm = o.obim_payload.sub(2);
        decode_zplane_chain_one(obim_bm, ow, oh,
                                /*dst_strip_off=*/o.x_strip,
                                /*overwrite=*/true,
                                target,
                                g.z_mask);
    }
}

// Invalidate the cache. Cheap — actual decode happens on first
// engine_zmask() request after this. Called on room change / drawObject
// / any composite mutation that affects the masks.
static void invalidate_zmask_cache() {
    g.num_zplanes = g.room.num_zplanes;
    if (g.num_zplanes < 0) g.num_zplanes = 0;
    if (g.num_zplanes > MAX_ZPLANES) g.num_zplanes = MAX_ZPLANES;
    g.z_mask_cached_plane = 0;
}

const uint8_t *engine_zmask(int plane_idx) {
    if (plane_idx <= 0 || plane_idx > g.num_zplanes) return nullptr;
    if (g.z_mask_cached_plane != plane_idx) {
        decode_plane_into_cache(plane_idx);
        g.z_mask_cached_plane = plane_idx;
    }
    return g.z_mask;
}
int engine_zmask_count()  { return g.num_zplanes; }
int engine_zmask_pitch()  { return MASK_BUF_PITCH; }
void engine_rebuild_zmasks() { invalidate_zmask_cache(); }

bool engine_change_room(int new_room) {
    if (new_room == 0) {
        // Room 0 is SCUMM's "no room" placeholder — boot scripts often
        // pass through it during init. Stay on whatever we have.
        g_vm.globals[VAR_ROOM] = 0;
        g_vm.room_change_pending = false;
        return true;
    }
    // Always update VAR_ROOM / VAR_ROOM_RESOURCE — even if we already
    // have the room data loaded — so the boot's pre-load (room 10) doesn't
    // leave VAR_ROOM stale across the first script-driven loadRoom.
    g_vm.globals[VAR_ROOM] = new_room;
    g_vm.globals[VAR_ROOM_RESOURCE] = new_room;
    if (new_room == g.current_room_id) {
        g_vm.room_change_pending = false;
        return true;
    }
    bool ok = room_load(new_room, g.master, &g.room);
    if (!ok) {
        platform::log("room transition: failed to load room %d\n", new_room);
        g_vm.room_change_pending = false;
        return false;
    }
    g.current_room_id = new_room;
    g.room_loaded = true;
    // Composite the room-wide backbuffer (background + objects). vscreen_main
    // is computed per-frame in engine_tick from a viewport into this.
    memset(g.vscreen_room, 0, sizeof(g.vscreen_room));
    room_load_palette(g.room, g.palette);
    // Reset the shadow-palette indirection on every room change. Mirrors
    // ScummEngine::startScene -> setCurrentPalette flow (palette.cpp:632)
    // which re-initialises the shadow table for the new room. Without
    // this, palette cycles from the previous room would "stick" into
    // the new room's palette range.
    for (int i = 0; i < 256; i++) g.shadow_palette[i] = (uint8_t)i;
    g.shadow_dirty = false;
    room_render_background(g.room, g.vscreen_room, ROOM_BUFFER_W);
    object_load_from_room(g.room.room_chunk, &g_object_table);
    refresh_object_states(&g_object_table);
    object_render_all(&g_object_table, g.vscreen_room, ROOM_BUFFER_W,
                      g.room.width, g.room.height);
    invalidate_zmask_cache();
    if (!g.room.boxd_payload.empty()) {
        walkbox_load(g.room.boxd_payload, Span{nullptr, 0}, &g.walkboxes);
    } else {
        memset(&g.walkboxes, 0, sizeof(g.walkboxes));
    }

    // Camera + camera vars — mirror ScummEngine::startScene
    // (room.cpp:202-216). Center the camera on screen-half, set MIN_X/MAX_X
    // bounds. cameraMoved() will be invoked by the per-frame loop.
    g.camera.mode    = kNormalCameraMode;
    g.camera.cur_x   = VIRTUAL_SCREEN_W / 2;
    g.camera.dest_x  = g.camera.cur_x;
    g.camera.last_x  = g.camera.cur_x;
    g.camera.movingToActor = 0;
    g.camera.follows = 0;
    g_vm.globals[VAR_CAMERA_MIN_X] = VIRTUAL_SCREEN_W / 2;
    g_vm.globals[VAR_CAMERA_MAX_X] = g.room.width - VIRTUAL_SCREEN_W / 2;
    if (g_vm.globals[VAR_CAMERA_MAX_X] < g_vm.globals[VAR_CAMERA_MIN_X])
        g_vm.globals[VAR_CAMERA_MAX_X] = g_vm.globals[VAR_CAMERA_MIN_X];
    // Trigger band — ScummVM scumm.cpp:2060-2064: camera._leftTrigger=10,
    // _rightTrigger=30 for v4+. (V0 uses 6/21.)
    g.camera.leftTrigger  = 10;
    g.camera.rightTrigger = 30;

    platform::log("room transition -> %d (%dx%d) camera=[%d..%d]\n",
                  new_room, g.room.width, g.room.height,
                  (int)g_vm.globals[VAR_CAMERA_MIN_X],
                  (int)g_vm.globals[VAR_CAMERA_MAX_X]);
    g_vm.room_change_pending = false;
    return true;
}

// Mirrors ScummEngine::cameraMoved (camera.cpp:169-193). Clamps cur_x and
// recomputes screenStartStrip / screenEndStrip and VAR_CAMERA_POS_X.
static void camera_moved() {
    int half_w = VIRTUAL_SCREEN_W / 2;
    if (g.camera.cur_x < half_w)
        g.camera.cur_x = half_w;
    else if (g.room_loaded && g.camera.cur_x > g.room.width - half_w)
        g.camera.cur_x = g.room.width - half_w;
    g.camera.screenStartStrip = g.camera.cur_x / 8 - NUM_STRIPS / 2;
    g.camera.screenEndStrip   = g.camera.screenStartStrip + NUM_STRIPS - 1;
    if (g.camera.screenStartStrip < 0) g.camera.screenStartStrip = 0;
    g_vm.globals[VAR_CAMERA_POS_X] = g.camera.cur_x;
}

// Mirrors ScummEngine::setCameraAt (camera.cpp:40-60). Clamps to MIN/MAX.
void engine_camera_set_at(int pos_x) {
    if (g.camera.mode != kFollowActorCameraMode ||
        (pos_x - g.camera.cur_x > VIRTUAL_SCREEN_W/2) ||
        (g.camera.cur_x - pos_x > VIRTUAL_SCREEN_W/2)) {
        g.camera.cur_x = pos_x;
    }
    g.camera.dest_x = pos_x;
    if (g.camera.cur_x < g_vm.globals[VAR_CAMERA_MIN_X])
        g.camera.cur_x = g_vm.globals[VAR_CAMERA_MIN_X];
    if (g.camera.cur_x > g_vm.globals[VAR_CAMERA_MAX_X])
        g.camera.cur_x = g_vm.globals[VAR_CAMERA_MAX_X];
    camera_moved();
}

// Mirrors ScummEngine::panCameraTo (camera.cpp:195-199).
void engine_camera_pan_to(int x) {
    g.camera.dest_x = x;
    g.camera.mode = kPanningCameraMode;
    g.camera.movingToActor = 0;
}

// Mirrors ScummEngine::setCameraFollows (camera.cpp:62-86). Switches the
// camera into follow-actor mode. If the actor isn't in the current room,
// triggers a room change (we issue a normal change_room). After the room
// is loaded, jumps the camera to the actor if the actor's strip is outside
// the trigger band, OR if `force` is set.
void engine_camera_set_follows(int actor_num, bool force) {
    g.camera.mode = kFollowActorCameraMode;
    g.camera.follows = actor_num;

    Actor *a = actor_get(actor_num);
    if (!a) return;

    if (a->room != g.current_room_id) {
        engine_change_room(a->room);
        g.camera.mode = kFollowActorCameraMode;
        g.camera.cur_x = a->x;
        engine_camera_set_at(g.camera.cur_x);
    }

    int t = a->x / 8 - g.camera.screenStartStrip;
    if (t < g.camera.leftTrigger || t > g.camera.rightTrigger || force) {
        engine_camera_set_at(a->x);
    }

    // ScummVM also marks every actor in the current room as needing redraw
    // and runs runInventoryScript(0) here. We composite every actor every
    // frame so the redraw flag is implicit; the inventory script is
    // out-of-scope until inventory rendering is wired up.
}

// Mirrors ScummEngine::moveCamera (camera.cpp:93-167). Walks cur_x toward
// dest_x in 8-px increments. In follow-actor mode, sets dest_x from the
// followed actor whenever the actor strays outside the trigger band.
static void camera_move_tick() {
    int pos = g.camera.cur_x;
    Actor *a = nullptr;
    int min_x = g_vm.globals[VAR_CAMERA_MIN_X];
    int max_x = g_vm.globals[VAR_CAMERA_MAX_X];

    g.camera.cur_x &= ~7;   // align to 8

    if (g.camera.cur_x < min_x) {
        g.camera.cur_x += 8;
        camera_moved();
        return;
    }
    if (g.camera.cur_x > max_x) {
        g.camera.cur_x -= 8;
        camera_moved();
        return;
    }

    if (g.camera.mode == kFollowActorCameraMode) {
        a = actor_get(g.camera.follows);
        if (a && a->room == g.current_room_id) {
            int actorx = a->x;
            int t = actorx / 8 - g.camera.screenStartStrip;
            if (t < g.camera.leftTrigger || t > g.camera.rightTrigger) {
                g.camera.movingToActor = 1;
            }
        }
    }

    if (g.camera.movingToActor) {
        a = actor_get(g.camera.follows);
        if (a && a->room == g.current_room_id) {
            g.camera.dest_x = a->x;
        }
    }

    if (g.camera.dest_x < min_x) g.camera.dest_x = min_x;
    if (g.camera.dest_x > max_x) g.camera.dest_x = max_x;

    if (g.camera.cur_x < g.camera.dest_x) g.camera.cur_x += 8;
    if (g.camera.cur_x > g.camera.dest_x) g.camera.cur_x -= 8;

    if (g.camera.movingToActor && a &&
        (g.camera.cur_x / 8) == (a->x / 8)) {
        g.camera.movingToActor = 0;
    }

    camera_moved();

    // VAR_SCROLL_SCRIPT support — when set non-zero, runs that script
    // each time camera moves with VAR_CAMERA_POS_X = cur_x.
    if (pos != g.camera.cur_x) {
        int scroll_script = g_vm.globals[VAR_SCROLL_SCRIPT];
        if (scroll_script) {
            g_vm.globals[VAR_CAMERA_POS_X] = g.camera.cur_x;
            int32_t args[1] = {0};
            vm_start_script(&g_vm, scroll_script, args, 0, false, false);
        }
    }
}

bool engine_init() {
    if (g.initialized) return true;

    // Populate the VM opcode dispatch table before anything tries to run a
    // script. Safe to call multiple times (idempotent overwrite).
    vm_opcodes_init();
    // MI1 VGA Floppy is v4 resource format — install v4-only opcodes
    // (ifState, ifNotState, pickupObjectOld, oldRoomEffect, saveLoadVars).
    vm_opcodes_v4_init();

    // Clear screen (palette index 0)
    memset(g.vscreen_main, 0, sizeof(g.vscreen_main));
    // Text overlay starts fully transparent — sentinel matches scummvm
    // gfx.h CHARSET_MASK_TRANSPARENCY (0xFD).
    memset(g.vscreen_text, 0xFD, sizeof(g.vscreen_text));

    // Default palette: grayscale ramp so the empty buffer is visible.
    for (int i = 0; i < 256; i++) {
        g.palette[i*3 + 0] = (uint8_t)i;
        g.palette[i*3 + 1] = (uint8_t)i;
        g.palette[i*3 + 2] = (uint8_t)i;
    }
    // Initialise _shadowPalette to identity (i -> i). Mirrors
    // ScummEngine::resetScummVars + setAmigaPaletteFromPtr / setupVGA
    // (palette.cpp:431-435). Game scripts mutate it via setupShadowPalette
    // / cyclePalette; without this initialisation, every pixel-blit would
    // remap to colour 0.
    for (int i = 0; i < 256; i++) g.shadow_palette[i] = (uint8_t)i;
    g.shadow_dirty = false;

    // Reset object-name pool — index of -1 == "no name".
    for (int i = 0; i < NUM_GLOBAL_OBJECTS; i++) g_obj_name_index[i] = -1;
    g_obj_name_count = 0;

    g.scale_mode = platform::ScaleMode::Fit;
    g.crop_x = 96;  // initial center for CROP
    g.crop_y = 36;
    g.frame = 0;
    g.quitting = false;

    // Parse master index
    Span master = platform::data_master_index();
    if (master.empty()) {
        platform::log("error: 000.LFL not loaded\n");
        return false;
    }
    if (!parse_master_index(master, &g.master)) {
        platform::log("error: failed to parse 000.LFL\n");
        return false;
    }
    resolve_room_offsets(&g.master);
    // Dump first 12 rooms post-resolve so we can see real offsets
    for (int i = 1; i <= 12 && i < g.master.num_rooms; i++) {
        if (g.master.rooms[i].disk != 0)
            platform::log("  resolved room %d -> disk %u offset 0x%08X\n",
                          i, g.master.rooms[i].disk, g.master.rooms[i].offset);
    }

    // Populate _objectStateTable / _objectOwnerTable from the global object
    // entries in 000.LFL. Mirrors ScummEngine_v4::readGlobalObjects
    // (resource_v4.cpp:241-275). Without this, every room object
    // defaults to state=0 in our table — but ScummVM ships with the
    // pre-set initial states encoded into the master index (e.g. cliff
    // clouds ON for room 10's title scene).
    int n_globals = g.master.num_global_objects;
    if (n_globals > NUM_GLOBAL_OBJECTS) n_globals = NUM_GLOBAL_OBJECTS;
    int nonzero = 0;
    for (int i = 0; i < n_globals; i++) {
        const GlobalObject &go = g.master.global_objects[i];
        g_object_state[i] = global_object_state(go);
        g_object_owner[i] = global_object_owner(go);
        if (g_object_state[i]) nonzero++;
    }
    platform::log("global objects: %d total, %d with non-zero initial state\n",
                  n_globals, nonzero);

    // Initialize actor pool before any boot script can manipulate them.
    actor_init_all();

    // Initialize text/charset state — _string[0..3], charset color map.
    // Charset 0 (901.LFL) is the main MI1 in-game font. ScummVM loads
    // it on demand via o5_resourceRoutines case 18 (LOAD_CHARSET); we
    // pre-load so the boot's "MI1 Floppy v1.0" banner can render.
    string_init();

    // TSB_ROOM env var: skip the boot script and force-load a specific
    // room for room-data exploration. Without it, the boot script does
    // its own loadRoom and we leave the framebuffer black until then —
    // mirrors ScummVM's startup (no pre-render before _bootScript runs).
    bool freeze_at_initial = false;
#ifndef THUMBY_DEVICE
    if (const char *env = getenv("TSB_ROOM")) {
        int r = atoi(env);
        if (r >= 0 && r < g.master.num_rooms && g.master.rooms[r].disk != 0) {
            freeze_at_initial = true;
            platform::log("TSB_ROOM=%d: forcing room, boot script disabled\n", r);
            engine_change_room(r);
            // For TSB_ROOM exploration, bypass the global state gate:
            // mark every loaded object visible so we can see the assets.
            for (int i = 1; i <= g_object_table.num_objects; i++) {
                ObjectData *o = &g_object_table.objects[i];
                if (o->obj_id > 0) o->state = 1;
            }
            object_render_all(&g_object_table, g.vscreen_room, ROOM_BUFFER_W,
                              g.room.width, g.room.height);
            invalidate_zmask_cache();
        }
    }
#endif
    g.skip_boot_script = freeze_at_initial;

    // Initialize audio: OPL2 emulator + AdLib MIDI driver + iMUSE sequencer
    // + mixer callback + platform audio device. Must be up before any boot
    // script can fire o5_startSound / o5_startMusic.
    {
        constexpr int kRequestedRate = 22050;
        opl2_init(kRequestedRate);
        adlib_init();
        imuse_init();
        int actual_rate = platform::audio_init(kRequestedRate, audio_mix_callback, nullptr);
        if (actual_rate <= 0) {
            platform::log("audio: platform::audio_init failed; running silent\n");
        } else {
            audio_mix_init(actual_rate);
            // Re-init OPL2 if rate differs so phase math matches.
            if (actual_rate != kRequestedRate) {
                opl2_init(actual_rate);
                adlib_init();
            }
            platform::log("audio: %d Hz mono\n", actual_rate);
        }
    }

    // Initialize VM and start the boot script (script 1).
    //
    // Initial global writes mirror ScummVM's resetScummVars() (vars.cpp:784)
    // followed by setupScummVars (game-version-specific). This is what fills
    // VAR_HEAPSPACE / VAR_FIXEDDISK / VAR_CHARINC / VAR_VIDEOMODE / etc. so
    // that the boot script's checks against those vars take the same path
    // as the reference implementation.
    vm_init(&g_vm);
    g_vm.globals[VAR_NUM_ACTOR]    = MAX_ACTORS - 1;
    g_vm.globals[VAR_MACHINE_SPEED] = 1;
    g_vm.globals[VAR_TIMER_NEXT]   = 0;
    // ScummVM starts _currentRoom at 0 until a script calls startScene; we
    // mirror that here so o5_loadRoom's same-room-shortcut (script_v5.cpp:
    // 1849) doesn't suppress the boot's first real room change.
    g_vm.globals[VAR_ROOM]         = 0;
    // resetScummVars() — applies for v4+ in MI1.
    g_vm.globals[VAR_HEAPSPACE]    = 1400;          // v4+
    g_vm.globals[VAR_FIXEDDISK]    = 1;             // v4+
    // The reference trace was captured with --debuglevel=2 which sets
    // _debugMode=true, so VAR_DEBUGMODE = 1. We hard-set 1 to match the
    // boot path the boot script takes when debug mode is on.
    g_vm.globals[VAR_DEBUGMODE]    = 1;
    g_vm.globals[VAR_CHARINC]      = 4;
    // VGA video mode -> 19 for the EGA/VGA renderers MI1 uses.
    g_vm.globals[VAR_VIDEOMODE]    = 19;
    // ScummVM auto-picks AdLib -> case MDT_ADLIB -> VAR_SOUNDCARD = 3
    // (vars.cpp:896). We emulate AdLib too via opl2/adlib so that's the
    // matching choice.
    g_vm.globals[VAR_SOUNDCARD]    = 3;

    if (!g.skip_boot_script) {
        int32_t boot_args[16] = {0};
        int slot = vm_start_script(&g_vm, 1, boot_args, 0, false, false);
        if (slot < 0) {
            platform::log("warning: failed to start boot script (script 1)\n");
        } else {
            platform::log("boot: started script 1 in slot %d\n", slot);
            g.boot_started = true;
        }
    } else {
        platform::log("boot: skipped (TSB_ROOM force mode)\n");
    }

    // Test path: probe sound IDs 1..199 to find the first one that's a
    // looping music track and start it - that lets us hear the title
    // theme even if the boot script hasn't fired o5_startMusic yet. An
    // actual o5_startMusic call will replace this.
    //
    // Heuristic: an AD-format sound is "music" if its kind byte is 0x80.
    // We prefer looping (play_once=0) over one-shots, but a one-shot
    // music track (MI1 sound 1 = LucasArts fanfare) is acceptable as a
    // fallback.
    {
        constexpr uint32_t kAdMinPayload = 2 + 0x11 + 8 * 16;
        int best_id = 0;        // looping music wins outright
        int fallback_id = 0;    // play-once music
        for (int s = 1; s <= 199; s++) {
            Span snd = resource_get_sound(s);
            if (snd.empty()) continue;
            const uint8_t *p = snd.data;
            uint32_t end = (uint32_t)snd.size;
            uint32_t ad_payload = 0, ad_size = 0;
            // Skip past optional SO wrapper, then walk WA/AD siblings.
            uint32_t off = 0;
            if (end >= 6 && p[4]=='S' && p[5]=='O') off = 6;
            while (off + 6 <= end) {
                uint32_t sz = (uint32_t)p[off]
                            | ((uint32_t)p[off+1] << 8)
                            | ((uint32_t)p[off+2] << 16)
                            | ((uint32_t)p[off+3] << 24);
                if (sz < 6 || sz > end - off) break;
                if (p[off+4]=='A' && p[off+5]=='D') {
                    ad_payload = off + 6;
                    ad_size    = sz - 6;
                    break;
                }
                if (p[off+4]=='S' && p[off+5]=='O') { off += 6; continue; }
                off += sz;
            }
            if (ad_payload == 0 || ad_size < kAdMinPayload) continue;
            uint8_t kind      = p[ad_payload + 2];
            uint8_t play_once = p[ad_payload + 4];
            if (kind != 0x80) continue;     // SFX
            if (play_once == 0) {
                if (best_id == 0) best_id = s;
                break;                      // looping music: stop scanning
            }
            if (fallback_id == 0) fallback_id = s;
        }
        int chosen = best_id ? best_id : fallback_id;
        if (chosen) {
            Span snd = resource_get_sound(chosen);
            if (imuse_start_sound(chosen, snd)) {
                platform::log("audio test: started sound %d (size=%zu, %s)\n",
                              chosen, snd.size,
                              best_id ? "looping music" : "play-once music");
            }
        } else {
            platform::log("audio test: no music sounds found in 1..199\n");
        }
    }

    g.initialized = true;
    return true;
}

bool engine_tick() {
    if (!g.initialized || g.quitting) return false;

    // -----------------------------------------------------------------------
    // Frame pacing — port of ScummEngine::waitForTimer (scumm.cpp:2802-2857)
    // and the surrounding go() loop (scumm.cpp:2702-2772).
    //
    // For SCUMM v4 (MI1 floppy DOS):
    //   _timerFrequency = PIT_BASE_FREQUENCY / PIT_V2_4_DIVISOR
    //                   = 1193182.0 / 5041.0 ≈ 236.6904 Hz
    //                   (scumm.cpp:2882, scumm.h:314-316)
    //   delta           = VAR(VAR_TIMER_NEXT), default 4 if 0/unset
    //                   (scumm.cpp:2704, 2717-2718)
    //   quarterFrames   = delta * 4
    //   msecDelay       = quarterFrames * (1000 / _timerFrequency)
    //                   (scumm.cpp:2804). With delta=4 ⇒ ~67.6 ms/frame
    //                   (~14.79 Hz), giving the music timer enough wall
    //                   time to advance between script polls.
    //
    // Anchored timing: _lastWaitTime becomes the *ideal* end time, not
    // the actual current time, so a frame that runs long is absorbed
    // into the next frame (scumm.cpp:2848-2856).
    //
    // The fractional-millisecond carry mirrors getIntegralTime
    // (scumm.cpp:2859-2868) so 67.609ms-per-frame doesn't accumulate as
    // a round-down bias of -0.6ms/frame.
    {
        constexpr double kTimerFrequency = 1193182.0 / 5041.0;  // v4 PIT
        int delta = (int)g_vm.globals[VAR_TIMER_NEXT];
        if (delta < 1) delta = 4;          // scumm.cpp:2704, 2717
        int quarterFrames = delta * 4;     // scumm.cpp:2772
        double fMsecs = (double)quarterFrames * (1000.0 / kTimerFrequency);

        static double s_msec_fract = 0.0;
        static uint32_t s_last_wait_time = 0;
        static bool s_pace_init = false;

        // getIntegralTime: accumulate fractional ms, round to whole ms.
        // (scumm.cpp:2859-2868)
        double whole;
        double frac = std::modf(fMsecs, &whole);
        s_msec_fract += frac;
        if (s_msec_fract >= 1.0) { s_msec_fract -= 1.0; whole += 1.0; }
        uint32_t msecDelay = (uint32_t)whole;

        uint32_t cur = platform::millis();
        if (!s_pace_init) {
            s_last_wait_time = cur;
            s_pace_init = true;
        }
        uint32_t diff = cur - s_last_wait_time;          // scumm.cpp:2812
        msecDelay = (msecDelay > diff) ? msecDelay - diff : 0;
        uint32_t endTime = cur + msecDelay;              // scumm.cpp:2814

        // 10ms-quantised poll; SDL audio thread is independent, host
        // events are pumped at the top of each engine_tick.
        // (scumm.cpp:2819-2846)
        while (true) {
            cur = platform::millis();
            if (cur >= endTime) break;
            uint32_t remain = endTime - cur;
            platform::sleep_ms(remain < 10u ? remain : 10u);
        }

        // Anchor at IDEAL end time, not actual cur, unless we overshot
        // by more than 50ms (suggesting a stall). (scumm.cpp:2856)
        s_last_wait_time = (cur > endTime + 50) ? cur : endTime;
    }

    platform::Input in{};
    if (!platform::poll_input(&in)) return false;

    // MENU tap cycles scale mode (placeholder - press detection to refine)
    if (in.menu_pressed) {
        g.scale_mode = (platform::ScaleMode)(((int)g.scale_mode + 1) % 3);
        const char *names[] = {"FIT", "FILL", "CROP"};
        platform::log("scale mode: %s\n", names[(int)g.scale_mode]);
    }

    // CROP pan with LB+dpad (placeholder)
    if (in.button_lb && g.scale_mode == platform::ScaleMode::Crop) {
        const int step = 4;
        if (in.dpad_left && g.crop_x > 0) g.crop_x -= step;
        if (in.dpad_right && g.crop_x < VIRTUAL_SCREEN_W - DISPLAY_W) g.crop_x += step;
        if (in.dpad_up && g.crop_y > 0) g.crop_y -= step;
        if (in.dpad_down && g.crop_y < VIRTUAL_SCREEN_H - DISPLAY_H) g.crop_y += step;
    }

    // Mirror ScummVM scummLoop (scumm.cpp:3081): refresh VAR_MUSIC_TIMER
    // from the iMUSE player BEFORE runAllScripts so the cutscene-timing
    // wait loops (e.g. Caribbean intro Script 149) can advance.
    int mt = imuse_get_music_timer();
    g_vm.globals[VAR_MUSIC_TIMER] = mt;
    static int last_mt = -1;
    if (mt != last_mt) { platform::log("MUSIC_TIMER=%d\n", mt); last_mt = mt; }

    // Mirror ScummVM scummLoop_updateScummVars (scumm.cpp:2989-3008).
    // delta is what ScummVM calls `_scummDeltaTime` — for v4/v5 a tick
    // is 1/60 sec, and the per-frame delta is VAR_TIMER_NEXT (default
    // 4 ticks ≈ 15 fps). Without this, "wait 6 ticks" loops in the
    // boot script never advance (audit H115).
    int dt = (int)g_vm.globals[VAR_TIMER_NEXT];
    if (dt <= 0) dt = 4;
    if (dt > 15) dt = 15;
    engine_update_scumm_vars(dt);

    // Run scripts for this frame
    vm_run_frame(&g_vm);

    // If a script requested a room change AND op_loadRoom hasn't already
    // performed it synchronously, do it here. (Most v4+ titles take the
    // synchronous path so entry/exit scripts can run nested.)
    if (g_vm.room_change_pending) {
        g_vm.room_change_pending = false;
        engine_change_room(g_vm.pending_room_id);
    }

    // VAR_MAIN_SCRIPT — runs every frame for games where it's defined.
    // Mirrors ScummEngine::scummLoop scumm.cpp:3178-3180:
    //     if (VAR_MAIN_SCRIPT != 0xFF && VAR(VAR_MAIN_SCRIPT) != 0)
    //         runScript(VAR(VAR_MAIN_SCRIPT), 0, 0, nullptr);
    // Upstream's `byte VAR_MAIN_SCRIPT = 0xFF` (scumm.h:1947) is the
    // "var not defined" sentinel; only later games assign it (e.g. v5+
    // index 127, vars.cpp:359). For v4 (MI1) it stays 0xFF so the dispatch
    // is a no-op. Hard-coding any other index here erroneously runs
    // whatever script number the boot stashed at that global slot.
    if (g_var_main_script != 0xFF) {
        int idx = g_var_main_script;
        int main_script = (int)g_vm.globals[idx];
        if (main_script) {
            int32_t no_args[1] = { 0 };
            vm_start_script(&g_vm, main_script, no_args, 0, false, false);
        }
    }

    // Sentence dispatch — mirrors checkAndRunSentenceScript (script.cpp:
    // 1166-1207). Pop the top _sentence[] entry and run the
    // VAR_SENTENCE_SCRIPT with (verb, objA, objB) when no sentence script
    // is already running.
    engine_sentence_tick();

    // Tick walking + animation BEFORE rendering, so position used for draw
    // is up-to-date.
    actor_tick_all(g.walkboxes.valid ? &g.walkboxes : nullptr);

    // Advance palette cycles (mirrors ScummEngine::cyclePalette, called
    // once per scumm loop). Sparkle / waterfall / lava effects rely on
    // this. The v4 path rotates the _shadowPalette indirection table —
    // the actual RGB triplets stay fixed; the next blit applies the
    // remap (audit F8).
    if (g.room_loaded) {
        for (int i = 0; i < 16; i++) {
            if (g.room.color_cycle[i].counter) { g.shadow_dirty = true; break; }
        }
        palette_cycle_tick(g.room.color_cycle, g.shadow_palette);
    }

    // Tick the camera (panCameraTo / clamp). Mirrors ScummEngine::moveCamera
    // (camera.cpp:93-167) called once per scummLoop_handleActors / scumm.cpp.
    if (g.room_loaded) {
        camera_move_tick();
    }

    // Per-frame composite: blit a 320-px viewport from the room-wide buffer
    // into vscreen_main, then draw actors on top in viewport-relative
    // coords. Mirrors ScummEngine::drawStripToScreen (gfx.cpp:630-705):
    // strips [_screenStartStrip..+39] from the main VirtScreen are blitted
    // to the real screen.
    if (g.room_loaded) {
        int x0 = g.camera.screenStartStrip * 8;
        if (x0 < 0) x0 = 0;
        if (x0 > ROOM_BUFFER_W - VIRTUAL_SCREEN_W)
            x0 = ROOM_BUFFER_W - VIRTUAL_SCREEN_W;
        if (g.shadow_dirty) {
            // Apply the _shadowPalette indirection during the blit.
            // Mirrors ScummEngine::drawStripToScreen which feeds the
            // shadow table through the room composite when copying to
            // the screen surface (palette.cpp:1670 / 1699).
            for (int y = 0; y < VIRTUAL_SCREEN_H; y++) {
                const uint8_t *src = g.vscreen_room + y * ROOM_BUFFER_W + x0;
                uint8_t *dst = g.vscreen_main + y * VIRTUAL_SCREEN_W;
                for (int x = 0; x < VIRTUAL_SCREEN_W; x++) {
                    dst[x] = g.shadow_palette[src[x]];
                }
            }
        } else {
            for (int y = 0; y < VIRTUAL_SCREEN_H; y++) {
                memcpy(g.vscreen_main + y * VIRTUAL_SCREEN_W,
                       g.vscreen_room + y * ROOM_BUFFER_W + x0,
                       VIRTUAL_SCREEN_W);
            }
        }
        // Actors are stored in room coordinates. actor_render_all subtracts
        // x_off from each actor's draw position so it lands in vscreen_main
        // at viewport-relative x.
        actor_render_all(g.vscreen_main, VIRTUAL_SCREEN_W,
                         g.walkboxes.valid ? &g.walkboxes : nullptr,
                         x0);
    }

    // Drive the talk-text state machine. Mirrors ScummEngine::displayDialog
    // called from scummLoop. When a talk message is active, this advances
    // _talkDelay and renders the next character into vscreen_text.
    string_tick();

    // The kTextVirtScreen overlay is composited inside platform::present
    // during scaling so 1-pixel glyph features survive the 320 -> 128
    // downsample with ink-priority sampling. We pass it as a parallel
    // buffer; vscreen_main remains text-free here.
    g.frame++;
    platform::present(g.vscreen_main, g.vscreen_text, g.palette,
                      g.scale_mode, g.crop_x, g.crop_y);

#ifndef THUMBY_DEVICE
    // Periodically dump the live virtual screen to PPM for offline inspection
    // (every 30 frames ≈ 1 second). Useful for verifying what's actually
    // visible while the boot script runs. Host-only: no fopen on device.
    if ((g.frame % 30) == 0) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/tsb_seq_%04u.ppm",
                 (unsigned)(g.frame / 30));
        FILE *f = fopen(path, "wb");
        if (!f) f = fopen("/tmp/tsb_vscreen.ppm", "wb");
        // Inline composite for the dump only — text overlay on top of
        // main scene, ignoring 0xFD sentinel. The live scaled output
        // uses the present() path (ink-priority + 2x2 blend) instead.
        auto dump_composited = [&](FILE *fp){
            fprintf(fp, "P6\n%d %d\n255\n",
                    VIRTUAL_SCREEN_W, VIRTUAL_SCREEN_H);
            for (int p = 0; p < VIRTUAL_SCREEN_W * VIRTUAL_SCREEN_H; p++) {
                uint8_t t = g.vscreen_text[p];
                uint8_t idx = (t != 0xFD) ? t : g.vscreen_main[p];
                fwrite(g.palette + idx*3, 3, 1, fp);
            }
        };
        if (f) { dump_composited(f); fclose(f); }
        // Also keep a "latest" copy for the existing tooling.
        f = fopen("/tmp/tsb_vscreen.ppm", "wb");
        if (f) { dump_composited(f); fclose(f); }
        // Also dump the full room-wide buffer for diagnosing strip layout.
        FILE *fr = fopen("/tmp/tsb_room.ppm", "wb");
        if (fr) {
            fprintf(fr, "P6\n%d %d\n255\n",
                    g.room_loaded ? g.room.width : ROOM_BUFFER_W,
                    VIRTUAL_SCREEN_H);
            int rw = g.room_loaded ? g.room.width : ROOM_BUFFER_W;
            for (int y = 0; y < VIRTUAL_SCREEN_H; y++) {
                for (int x = 0; x < rw; x++) {
                    uint8_t idx = g.vscreen_room[y * ROOM_BUFFER_W + x];
                    fwrite(g.palette + idx*3, 3, 1, fr);
                }
            }
            fclose(fr);
        }
    }
#endif
    return true;
}

void engine_shutdown() {
    g.quitting = true;
}

}  // namespace tsb
