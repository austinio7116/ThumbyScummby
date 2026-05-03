// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// Object loading + render.

#include "object.h"
#include "small_chunk.h"
#include "smap.h"
#include "platform.h"

#include <string.h>

namespace tsb {

void object_init(ObjectTable *t) {
    memset(t, 0, sizeof(*t));
}

// V4 small-header OBCD (OC chunk) — mirrors ScummVM_v4::resetRoomObject()
// (object.cpp:1026). `ptr` there points at the OBCD chunk start (full chunk
// including 6-byte header). Our `obcd_payload` is the payload AFTER that
// header, so all offsets below are -6 relative to ScummVM's `ptr`.
//
// Layout (offsets in obcd_payload):
//   p+0..1   obj_id           (uint16 LE)
//   p+2      unknown (cdhd version byte)
//   p+3      x                (in 8-pixel units; x_pos = p[3] * 8)
//   p+4      y                (lower 7 bits in 8-pixel units; top bit = parentstate)
//   p+5      w                (in 8-pixel units; width = p[5] * 8)
//   p+6      parent
//   p+7..8   walk_x           (int16 LE, in pixels)
//   p+9..10  walk_y           (int16 LE, in pixels)
//   p+11     actordir(low 3) | height(high 5 — i.e. height_pixels = p[11] & 0xF8)
//   p+12...  verb-script directory + OBNA name
//
// Note: there is NO standalone 'flags' byte and NO standalone 'h' byte in
// v4; height and actordir are packed into a single byte. The previous
// parser was off-by-one starting from p+5 onward, which is why h came out
// as zero for every object and walk_y/actordir read garbage.
static void parse_obcd_v4(Span obcd_payload, ObjectData *o) {
    if (obcd_payload.size < 12) return;
    const uint8_t *p = obcd_payload.data;
    o->obj_id      = read_le16(p + 0);
    // p[2] = cdhd version / unknown
    o->x_strip     = p[3];
    o->y           = (uint8_t)(p[4] & 0x7F);          // y in 8-pixel units
    o->parentstate = (uint8_t)((p[4] & 0x80) ? 1 : 0);
    o->w_strip     = p[5];
    o->parent      = p[6];
    o->walk_x      = read_le16s(p + 7);
    o->walk_y      = read_le16s(p + 9);
    uint8_t dh     = p[11];
    o->actor_dir   = (uint8_t)(dh & 0x07);
    o->h           = (uint8_t)((dh & 0xF8) / 8);      // height in 8-pixel units
}

int object_load_from_room(Span room_chunk_payload, ObjectTable *t) {
    object_init(t);

    // Walk all sub-chunks; collect OI (OBIM) and OC (OBCD) by obj_id.
    // For v4 small_header layout, each OI is followed (eventually) by its
    // OC; collect both into ObjectData entries keyed by obj_id.
    //
    // Slot allocation mirrors ScummVM: slot 0 is reserved (empty), local
    // objects live in slots 1..N. The on-disk OBCD `parent` byte encodes
    // a slot index in this 1-based scheme — parent == 0 means "no parent".

    Span obim_buf[MAX_OBJECTS]; int obim_count = 0;
    int  obim_id [MAX_OBJECTS];

    int next_slot = 1;
    size_t cur = 0;
    SmallChunk c{};
    while (small_next(room_chunk_payload, &cur, &c)) {
        if (c.tag == small_tag('O','I')) {
            // OBIM payload starts with: uint16 obj_id, uint16 num_imgs, uint8 [...]
            if (c.payload.size < 4) continue;
            int oid = read_le16(c.payload.data);
            if (obim_count < MAX_OBJECTS) {
                obim_id[obim_count]  = oid;
                obim_buf[obim_count] = c.payload;
                obim_count++;
            }
        } else if (c.tag == small_tag('O','C')) {
            if (next_slot >= MAX_OBJECTS) break;
            ObjectData *o = &t->objects[next_slot++];
            memset(o, 0, sizeof(*o));
            o->obcd_payload = c.payload;
            parse_obcd_v4(c.payload, o);
            // State is initialised from the global object-state table by the
            // caller (engine_change_room). Default 0 here so a slot that has
            // never been state-set stays invisible — matches ScummVM, where
            // _objectStateTable is zero-initialised at game start.
            o->state = 0;
            // try to match an OBIM by obj_id
            for (int i = 0; i < obim_count; i++) {
                if (obim_id[i] == o->obj_id) {
                    o->obim_payload = obim_buf[i];
                    break;
                }
            }
        }
    }
    t->num_objects = next_slot - 1;   // number populated; slots 1..num_objects
    platform::log("objects: loaded %d (OIs scanned: %d)\n",
                  t->num_objects, obim_count);
    return t->num_objects;
}

ObjectData *object_get_by_id(ObjectTable *t, int obj_id) {
    for (int i = 1; i <= t->num_objects; i++) {
        if (t->objects[i].obj_id == obj_id) return &t->objects[i];
    }
    return nullptr;
}

void object_mark_all_dirty(ObjectTable *t) {
    // ScummVM uses dirty-rect tracking so changing an object's state
    // can mark only its strips for redraw on the next frame
    // (gfx.cpp:1108 testGfxUsageBit / addDirtyRect). Our pipeline
    // recomposites the room-wide buffer + viewport every frame
    // unconditionally, so per-object dirty bits would never gate any
    // work. The function stays as a public hook so callers compile
    // unchanged; it is intentionally a no-op for our render model.
    (void)t;
}

// V4 small-header OBIM (OI chunk) layout — mirrors ScummVM's
// getObjectImage() which does `ptr += 8` for GF_SMALL_HEADER (object.cpp:1408).
// `ptr` there is the full chunk start, so +8 = +6 (chunk hdr) +2 (obj_id),
// landing on the SMAP-style image header.
//
//   uint16 LE  obj_id            (offset 0 in payload)
//   uint32 LE  smap_len/zplane   (offset 2 — same role as room BM's first uint32)
//   uint32 LE  strip_offsets[w]  (relative to start of "smap_len" word)
//   ...        strip-encoded pixel data
//
// For small_header v4 there is exactly ONE bitmap per OBIM (the `state`
// argument is ignored; state-driven appearance changes are handled by the
// scripts toggling separate OBIM chunks). So we always render the single
// SMAP body sitting at obim_payload + 2.

// Mirrors ScummVM ScummEngine::drawRoomObject (object.cpp:600-618). For v4
// the state mask is 0xF. We walk up the parent chain: the object draws
// only if every ancestor's (state & 0xF) equals the immediate child's
// `parentstate`. A top-level object (parent == 0) draws unconditionally
// (the caller's `state & mask` gate has already filtered it).
//
// Clipping: the destination is the room-wide composite buffer of width
// `buf_w` (== ScummEngine `_roomWidth`), NOT the 320-pixel viewport.
// Earlier versions of this code clamped to VIRTUAL_SCREEN_W/H, which
// silently dropped right-side scenery in any room wider than 320 px
// (audit F6 / scummvm-upstream/engines/scumm/object.cpp:600+).
static void draw_one_object(const ObjectTable *t, int slot,
                            uint8_t *vscreen_back, int pitch,
                            int buf_w, int buf_h) {
    constexpr int mask = 0x0F;
    const ObjectData *od = &t->objects[slot];
    if (slot < 1 || od->obj_id < 1 || !od->state) return;

    // Ancestor-chain visibility check.
    while (true) {
        uint8_t a = od->parentstate;
        if (od->parent == 0) break;            // top — proceed to draw
        const ObjectData *p = &t->objects[od->parent];
        if ((p->state & mask) != a) return;    // ancestor mismatch — hide
        od = p;
    }

    // od now points back at the child (we broke when parent==0). Re-read
    // the slot we were asked to draw — `od` got walked.
    od = &t->objects[slot];

    if (od->obim_payload.size < 6) return;
    if (od->w_strip == 0 || od->h == 0) return;

    int x_pix = od->x_strip * 8;
    int y_pix = od->y * 8;
    int w_pix = od->w_strip * 8;
    int h_pix = od->h * 8;
    if (x_pix < 0 || y_pix < 0) return;
    if (w_pix <= 0 || h_pix <= 0) return;
    if (x_pix >= buf_w || y_pix >= buf_h) return;
    if (x_pix + w_pix > buf_w) return;
    if (y_pix + h_pix > buf_h) return;

    Span bm = od->obim_payload.sub(2);
    uint8_t *dst = vscreen_back + (size_t)y_pix * pitch + x_pix;
    smap_decode_bm(bm, w_pix, h_pix, dst, pitch);
}

// Mirrors ScummEngine::drawRoomObjects (object.cpp:620-645). v4 path:
// iterate slots N..1 in reverse, gated by (state & 0xF).
void object_render_all(const ObjectTable *t,
                       uint8_t *vscreen_back, int pitch,
                       int buf_w, int buf_h) {
    constexpr int mask = 0x0F;
    if (buf_w <= 0) buf_w = pitch;
    if (buf_h <= 0) buf_h = VIRTUAL_SCREEN_H;
    for (int i = t->num_objects; i >= 1; i--) {
        const ObjectData *o = &t->objects[i];
        if (o->obj_id > 0 && (o->state & mask)) {
            draw_one_object(t, i, vscreen_back, pitch, buf_w, buf_h);
        }
    }
}

void object_draw_single(const ObjectTable *t, int obj_id,
                        uint8_t *vscreen_back, int pitch,
                        int buf_w, int buf_h) {
    if (!t || obj_id <= 0) return;
    if (buf_w <= 0) buf_w = pitch;
    if (buf_h <= 0) buf_h = VIRTUAL_SCREEN_H;
    for (int i = t->num_objects; i >= 1; i--) {
        const ObjectData *o = &t->objects[i];
        if (o->obj_id == obj_id) {
            draw_one_object(t, i, vscreen_back, pitch, buf_w, buf_h);
            return;
        }
    }
}

int object_find_at(const ObjectTable *t, int x, int y) {
    for (int i = t->num_objects; i >= 1; i--) {
        const ObjectData *o = &t->objects[i];
        if (o->state == 0) continue;
        int x0 = o->x_strip * 8, y0 = o->y * 8;
        int x1 = x0 + o->w_strip * 8, y1 = y0 + o->h * 8;
        if (x >= x0 && x < x1 && y >= y0 && y < y1) {
            return o->obj_id;
        }
    }
    return 0;
}

}  // namespace tsb
