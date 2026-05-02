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
struct CostumeData {
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

bool costume_parse(Span resource, CostumeData *out);

// Render one limb image at (dx, dy) into the 320x200 8bpp main virtual
// screen.
//
//  cost          — parsed costume.
//  limb_idx      — 0..15.
//  cel_index     — index into the limb's frame table (the value pulled from
//                  the actor's CostumeData::curpos, masked & 0x7F via the
//                  command stream lookup). For now we treat it as a direct
//                  cel index; the caller can pass either the animation-
//                  command-resolved value or the raw frame number.
//  dx, dy        — actor screen position in 320x200 virtual coords.
//  scale_x/y     — 0..255 (255 = full size).
//  flip_x        — true to mirror horizontally.
//  actor_palette — maps 0..(num_colors-1) onto screen palette indices.
//                  Pass nullptr to use the costume's own palette table.
//  mask_buf      — 1bpp z-plane, 8 px per byte, packed in vertical strips
//                  of (width/8) bytes per row. Pass nullptr to skip masking.
//  num_strips    — strips per row of mask_buf (= virtual screen width / 8).
//  vscreen       — destination 8bpp buffer.
//  vscreen_pitch — bytes per row of vscreen (typically 320).
//  transparent_color — palette index treated as transparent (no write).
//
// Out-of-bounds and zero-size cels are silently skipped.
void costume_render_limb(const CostumeData *cost, int limb_idx, int cel_index,
                         int dx, int dy, int scale_x, int scale_y, bool flip_x,
                         const uint8_t *actor_palette,
                         const uint8_t *mask_buf, int num_strips,
                         uint8_t *vscreen, int vscreen_pitch,
                         uint8_t transparent_color);

}  // namespace tsb
