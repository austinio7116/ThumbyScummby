// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — SCUMM v4/v5 costume parser + ByleRLE renderer.
//
// A COST (costume) resource describes an animated character composed of up
// to 16 limbs. Each limb has its own animation track (sequence of cel
// indices); each cel is a ByleRLE-compressed image with relative-position
// metadata. The renderer composes one or more limb cels at the actor's
// screen position, applying scale, mirror, palette remap, and optional
// z-plane masking.
//
// Reference: scummvm-upstream/engines/scumm/{costume,base-costume}.cpp.

#pragma once

#include "types.h"

namespace tsb {

// Parsed view onto a COST resource. All pointers reference XIP data — no
// copy. `resource` is the COST chunk PAYLOAD (the bytes after the 6-byte
// small_header). The format-specific offsets follow the layout described
// in spec_03_actor_costume.md §4 and the v4 ScummVM loader at
// costume.cpp:417-493.
struct ParsedCostume {
    Span           resource;        // payload of the COST small-chunk
    const uint8_t *baseptr;         // base for offset arithmetic (= resource.data)
    uint8_t        num_anim;        // number of animations
    uint8_t        format;          // 0x58 / 0x59 (low 7 bits)
    uint8_t        num_colors;      // 16 or 32 from format
    bool           mirror;          // bit 7 of format byte
    const uint8_t *palette;         // 16 or 32 bytes
    const uint8_t *anim_cmds;       // command stream (per-frame command bytes)
    const uint8_t *frame_offsets;   // 16-entry table of LE16: limb -> frameptr offset
    const uint8_t *data_offsets;    // num_anim 2-byte entries: anim -> table offset

    bool valid;                     // false if parse failed
};

bool costume_parse(Span resource, ParsedCostume *out);

// Forward — keep includes light. Defined in actor.h.
struct Actor;

// Mirrors ClassicCostumeLoader::costumeDecodeData (costume.cpp:722-795).
// For an actor, parse anim record at _dataOffsets[anim*2], walk the
// 16-bit usemask, and update _cost.curpos / start / end / frame for
// each requested limb. Special markers 0x79 (stop limb) and 0x7A
// (start limb) toggle the stopped_mask bit.
void costume_decode_data(Actor *a, int frame, unsigned usemask);

// Mirrors ClassicCostumeLoader::increaseAnim (costume.cpp:849-901). For
// a single limb slot, advance curpos through the animation command
// stream until the next valid drawable command. Returns true if the
// limb's draw command changed.
bool costume_increase_anim(Actor *a, int slot);

// Mirrors ClassicCostumeLoader::increaseAnims (costume.cpp:839-847).
// Iterates all 16 limbs.
bool costume_increase_anims(Actor *a);

// ScummVM util.cpp:41-49 — direction (0..359) -> 0/1/2/3 (W/E/S/N).
int  costume_new_dir_to_old(int dir);

// Render one limb image at (dx, dy) into the 320x200 8bpp main virtual
// screen.
//
// `xmove_io`/`ymove_io` are the per-actor xMove/yMove accumulators
// (mirrors ScummVM ClassicCostumeRenderer::{_xMove,_yMove}, costume.cpp:
// 638-644). On entry they hold the running offset built up by previous
// limbs in the same costume draw call; on exit we add this cel's
// `moveX` (and subtract `moveY`) so subsequent limbs anchor relative
// to the same chain.
//
//  cost          — parsed costume.
//  limb_idx      — 0..15.
//  cel_index     — index into the limb's frame table (the value pulled from
//                  the actor's ParsedCostume::curpos, masked & 0x7F via the
//                  command stream lookup). For now we treat it as a direct
//                  cel index; the caller can pass either the animation-
//                  command-resolved value or the raw frame number.
//  dx, dy        — actor screen position in 320x200 virtual coords.
//  scale_x/y     — 0..255 (255 = full size).
//  flip_x        — true to mirror horizontally.
//  actor_palette — maps 0..(num_colors-1) onto screen palette indices.
//                  Pass nullptr to use the costume's own palette table.
//  mask_buf      — 1bpp z-plane, 8 px per byte, packed in vertical strips
//                  of `mask_pitch` bytes per row. Pass nullptr to skip masking.
//                  The mask is room-wide: lookup x = dst_x + mask_x_off.
//  mask_pitch    — row stride of mask_buf in strips (== ROOM_BUFFER_W / 8
//                  for the engine's room-wide masks).
//  mask_x_off    — added to destination x before mask lookup. For viewport-
//                  relative actor draws this should be the camera's room-x
//                  offset (`screenStartStrip * 8`); for room-wide compositing
//                  pass 0.
//  vscreen       — destination 8bpp buffer.
//  vscreen_pitch — bytes per row of vscreen (typically 320).
//  transparent_color — palette index treated as transparent (no write).
//
// Out-of-bounds and zero-size cels are silently skipped.
void costume_render_limb(const ParsedCostume *cost, int limb_idx, int cel_index,
                         int dx, int dy, int scale_x, int scale_y, bool flip_x,
                         const uint8_t *actor_palette,
                         const uint8_t *mask_buf, int mask_pitch,
                         int mask_x_off,
                         uint8_t *vscreen, int vscreen_pitch,
                         uint8_t transparent_color,
                         int *xmove_io = nullptr, int *ymove_io = nullptr);

}  // namespace tsb
