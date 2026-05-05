// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — SCUMM v4/v5 costume parser + ByleRLE renderer.
//
// Mirrors the algorithms in scummvm-upstream/engines/scumm/{costume,
// base-costume}.cpp, distilled for the small-header (v4) layout used by
// Monkey Island 1 VGA Floppy.
//
// COST resource payload (after the 6-byte small_header):
//
//    +0  uint8[6]  reserved / unused (in v4 small-header the loader skips
//                  to ptr+0 directly)
//    +6  uint8     num_anim
//    +7  uint8     format byte (low 7 bits = format code, bit 7 = mirror)
//    +8  uint8[N]  costume palette (N = 16 if format==0x58, 32 if 0x59)
//   +8+N+0  uint16 LE   anim_cmds_offs (offset to command stream)
//   +8+N+2  uint16 LE * 16   limb_frame_offs (per-limb frame table offsets)
//   +8+N+34 uint16 LE * num_anim   anim_offs (per-anim record offsets)
//   ...
//
// In our parser:
//    baseptr        = payload start (== resource.data)
//    frame_offsets  = baseptr + 8 + N + 2
//    data_offsets   = baseptr + 8 + N + 34
//    anim_cmds      = baseptr + READ_LE16(baseptr + 8 + N)
//
// Cel data (one image / limb frame):
//
//    uint16 LE width
//    uint16 LE height
//    int16  LE rel_x
//    int16  LE rel_y
//    int16  LE move_x
//    int16  LE move_y
//    uint8  redir_limb       (Indy3 / SamMax — usually 0xFF)
//    uint8  redir_pict       (...)
//    uint8[] byleRLE stream
//
// We decode the image directly into the main virtual screen. The ByleRLE
// inner loop matches base-costume.cpp:286-421.

#include "costume.h"
#include "actor.h"
#include "resource.h"
#include "platform.h"

#include <string.h>

namespace tsb {

// 256-entry threshold table, identical to ScummVM smallCostumeScaleTable.
// Used to determine which row/column of the costume image to skip when the
// actor is scaled down. value <= scale → keep the row/column.
static const uint8_t kSmallScaleTable[256] = {
    0xFF, 0xFD, 0x7D, 0xBD, 0x3D, 0xDD, 0x5D, 0x9D,
    0x1D, 0xED, 0x6D, 0xAD, 0x2D, 0xCD, 0x4D, 0x8D,
    0x0D, 0xF5, 0x75, 0xB5, 0x35, 0xD5, 0x55, 0x95,
    0x15, 0xE5, 0x65, 0xA5, 0x25, 0xC5, 0x45, 0x85,
    0x05, 0xF9, 0x79, 0xB9, 0x39, 0xD9, 0x59, 0x99,
    0x19, 0xE9, 0x69, 0xA9, 0x29, 0xC9, 0x49, 0x89,
    0x09, 0xF1, 0x71, 0xB1, 0x31, 0xD1, 0x51, 0x91,
    0x11, 0xE1, 0x61, 0xA1, 0x21, 0xC1, 0x41, 0x81,
    0x01, 0xFB, 0x7B, 0xBB, 0x3B, 0xDB, 0x5B, 0x9B,
    0x1B, 0xEB, 0x6B, 0xAB, 0x2B, 0xCB, 0x4B, 0x8B,
    0x0B, 0xF3, 0x73, 0xB3, 0x33, 0xD3, 0x53, 0x93,
    0x13, 0xE3, 0x63, 0xA3, 0x23, 0xC3, 0x43, 0x83,
    0x03, 0xF7, 0x77, 0xB7, 0x37, 0xD7, 0x57, 0x97,
    0x17, 0xE7, 0x67, 0xA7, 0x27, 0xC7, 0x47, 0x87,
    0x07, 0xEF, 0x6F, 0xAF, 0x2F, 0xCF, 0x4F, 0x8F,
    0x0F, 0xDF, 0x5F, 0x9F, 0x1F, 0xBF, 0x3F, 0x7F,
    0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0,
    0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
    0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8,
    0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
    0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4,
    0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
    0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC,
    0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
    0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2,
    0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
    0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA,
    0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
    0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6,
    0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
    0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE,
    0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE
};

// ---------------------------------------------------------------------------
// Parse a COST resource payload. The payload is what small_chunk gives us
// (i.e. the bytes after the 6-byte LE-size + tag header).
//
// In ScummVM's loadCostume() the layout for SMALL_HEADER (v4) games is:
//   ptr += 0  (no skip)
//   _baseptr  = ptr
//   _numAnim  = ptr[6]
//   _format   = ptr[7] & 0x7F
//   _mirror   = (ptr[7] & 0x80) != 0
//   _palette  = ptr + 8                       (N bytes)
//   _frameOffsets = (ptr + 8 + N) + 2         (skips the cmds offset)
//   _animCmds = _baseptr + READ_LE16(ptr + 8 + N)
//   _dataOffsets  = (ptr + 8 + N) + 34        (16 frame offsets * 2 bytes)
// ---------------------------------------------------------------------------
bool costume_parse(Span resource, CostumeData *out) {
    memset(out, 0, sizeof(*out));
    if (resource.size < 8) {
        platform::log("costume_parse: resource too small (%zu bytes)\n",
                      resource.size);
        return false;
    }
    out->resource  = resource;
    out->baseptr   = resource.data;
    out->num_anim  = resource.data[6];
    uint8_t fmt    = resource.data[7];
    out->format    = (uint8_t)(fmt & 0x7F);
    out->mirror    = (fmt & 0x80) != 0;

    int n = 0;
    switch (out->format) {
        case 0x58: n = 16; break;
        case 0x59: n = 32; break;
        default:
            platform::log("costume_parse: unsupported format 0x%02X\n",
                          out->format);
            // Allow fallback: treat as 16-color so we don't crash; real cels
            // probably won't decode but the caller can no-op.
            n = 16;
            break;
    }
    out->num_colors = (uint8_t)n;

    if (resource.size < (size_t)8 + (size_t)n + 2 + 32) {
        platform::log("costume_parse: truncated header (size=%zu, n=%d)\n",
                      resource.size, n);
        return false;
    }
    out->palette       = resource.data + 8;
    const uint8_t *p   = resource.data + 8 + n;
    uint16_t cmds_offs = read_le16(p);
    out->frame_offsets = p + 2;       // 16 limb frame-table offsets
    out->data_offsets  = p + 34;      // num_anim animation-record offsets

    if (cmds_offs >= resource.size) {
        platform::log("costume_parse: cmds_offs 0x%X >= size %zu\n",
                      cmds_offs, resource.size);
        out->anim_cmds = nullptr;
    } else {
        out->anim_cmds = resource.data + cmds_offs;
    }

    out->valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// ByleRLE inner decoder. Stream is a sequence of (control, [extended-len])
// runs that emit pixels in column-major order across width × height.
//
//   control = src[0]
//   color   = control >> shr            ; shr depends on numColors
//   length  = control & mask            ; mask = (1<<shr)-1
//   if length == 0: length = src[1]; src += 1
//   src += 1
//
// Then output `length` pixels of `color`, advancing destination 1 row each
// pixel. When `height` rows exhausted, advance to next column. Stop when
// `width` columns done.
//
// Color 0 is transparent (do not write). Each pixel is mapped through the
// actor's palette before write.
//
// Scaling: when scale_x/y < 0xFF, columns/rows are skipped according to a
// 256-entry scale table — keep the row/column iff scaleTable[idx] < scale.
// `idx` advances by 1 every row/column in the source, regardless of skips
// (matches v4 disasm; v5+ uses scaleXStep increment instead).
// ---------------------------------------------------------------------------
static void byle_rle_decode(
    const uint8_t *src, const uint8_t *src_end,
    int width, int height,
    int dst_x, int dst_y,
    int scale_x, int scale_y,
    int x_step,                         // +1 for normal, -1 for flipped
    int shr, int mask_bits,
    const uint8_t *actor_palette,       // 32-byte; 0xFF = "use cost palette"
    const uint8_t *cost_palette,        // costume's intrinsic palette
    const uint8_t *mask_buf, int mask_pitch, int mask_x_off,
    uint8_t *vscreen, int vscreen_pitch,
    int clip_w, int clip_h,
    uint8_t transparent_color)
{
    if (!src || src >= src_end) return;
    if (width <= 0 || height <= 0) return;

    // scale table indexes
    int sx_idx = 0;
    int sy_idx_start = 0;
    int sy_idx = 0;

    // pixel position
    int x = dst_x;
    int y = dst_y;

    // run state
    int     run_len   = 0;
    uint8_t run_color = 0;

    int columns_left = width;

    // Pre-fetch first run
    if (src >= src_end) return;
    uint8_t c0 = *src++;
    run_color = (uint8_t)(c0 >> shr);
    run_len   = (uint8_t)(c0 & mask_bits);
    if (run_len == 0) {
        if (src >= src_end) return;
        run_len = *src++;
    }

    while (columns_left > 0) {
        // For each source row in this column
        for (int row = 0; row < height; row++) {
            // Decide whether to keep this row (scale_y test)
            bool keep_row = (scale_y >= 0xFF) ||
                            (kSmallScaleTable[sy_idx & 0xFF] < scale_y);
            sy_idx++;

            if (keep_row) {
                if (run_color != 0 &&
                    x >= 0 && x < clip_w &&
                    y >= 0 && y < clip_h) {
                    bool masked = false;
                    if (mask_buf) {
                        // Mask is room-wide; viewport-relative draw x is
                        // shifted into room-x for the lookup.
                        int mx = x + mask_x_off;
                        int bx = mx >> 3;
                        int bb = 0x80 >> (mx & 7);
                        if (bx >= 0 && bx < mask_pitch && y >= 0)
                            masked = (mask_buf[y * mask_pitch + bx] & bb) != 0;
                    }
                    if (!masked) {
                        // Mirrors ScummVM ClassicCostumeRenderer::setPalette
                        // (cost.cpp:817-823): when actor's per-color override
                        // is 0xFF, fall back to the costume's palette entry.
                        uint8_t out_pix = actor_palette ? actor_palette[run_color] : 0xFF;
                        if (out_pix == 0xFF && cost_palette)
                            out_pix = cost_palette[run_color];
                        if (out_pix != transparent_color) {
                            vscreen[y * vscreen_pitch + x] = out_pix;
                        }
                    }
                }
                y++;
            }

            // Consume one pixel from current run
            if (--run_len == 0) {
                if (src >= src_end) return;
                uint8_t c = *src++;
                run_color = (uint8_t)(c >> shr);
                run_len   = (uint8_t)(c & mask_bits);
                if (run_len == 0) {
                    if (src >= src_end) return;
                    run_len = *src++;
                }
            }
        }

        // End of column — decide whether to advance x
        bool keep_col = (scale_x >= 0xFF) ||
                        (kSmallScaleTable[sx_idx & 0xFF] < scale_x);
        sx_idx++;
        if (keep_col) x += x_step;

        // Reset y to top
        y = dst_y;
        sy_idx = sy_idx_start;

        columns_left--;
    }
}

// ---------------------------------------------------------------------------
// Resolve a limb's frame data within the costume resource and dispatch to
// the ByleRLE decoder.
// ---------------------------------------------------------------------------
void costume_render_limb(const CostumeData *cost, int limb_idx, int cel_index,
                         int dx, int dy, int scale_x, int scale_y, bool flip_x,
                         const uint8_t *actor_palette,
                         const uint8_t *mask_buf, int mask_pitch,
                         int mask_x_off,
                         uint8_t *vscreen, int vscreen_pitch,
                         uint8_t transparent_color,
                         int *xmove_io, int *ymove_io)
{
    if (!cost || !cost->valid) return;
    if (limb_idx < 0 || limb_idx >= 16) return;
    if (cel_index < 0) return;

    const uint8_t *res_end = cost->resource.data + cost->resource.size;

    // frame_offsets is 16 LE16's; one per limb. It gives us the offset into
    // the resource where the limb's per-cel offset table starts.
    const uint8_t *fo_entry = cost->frame_offsets + limb_idx * 2;
    if (fo_entry + 2 > res_end) return;
    uint16_t limb_table_off = read_le16(fo_entry);
    if (limb_table_off == 0) return;   // empty limb
    if (cost->baseptr + limb_table_off >= res_end) return;
    const uint8_t *limb_table = cost->baseptr + limb_table_off;

    // The cel_index is masked to 7 bits when read from the command stream
    // (commands have a sentinel 0x7B = "no draw"). Caller can pre-resolve.
    int code = cel_index & 0x7F;
    if (code == 0x7B) return;

    // Each limb's table is an array of LE16 cel offsets indexed by code.
    // Bounds: we trust the upstream caller to supply a valid code.
    const uint8_t *cel_off_entry = limb_table + code * 2;
    if (cel_off_entry + 2 > res_end) return;
    uint16_t cel_off = read_le16(cel_off_entry);
    if (cel_off == 0) return;
    const uint8_t *cel = cost->baseptr + cel_off;
    if (cel + 12 > res_end) return;

    // Cel header: width, height, rel_x, rel_y, move_x, move_y (all LE16).
    // Mirrors ScummVM ClassicCostumeRenderer::mainDraw (costume.cpp:638-644):
    //   xmoveCur = _xMove + relX;
    //   ymoveCur = _yMove + relY;
    //   _xMove += moveX;  _yMove -= moveY;
    uint16_t cw   = read_le16(cel + 0);
    uint16_t ch   = read_le16(cel + 2);
    int16_t  rx   = read_le16s(cel + 4);
    int16_t  ry   = read_le16s(cel + 6);
    int16_t  mx   = read_le16s(cel + 8);
    int16_t  my   = read_le16s(cel + 10);
    const uint8_t *src = cel + 12;

    int xmove_pre = xmove_io ? *xmove_io : 0;
    int ymove_pre = ymove_io ? *ymove_io : 0;
    if (xmove_io) *xmove_io += mx;
    if (ymove_io) *ymove_io -= my;

    if (cw == 0 || ch == 0) return;
    if (cw > 320 || ch > 200) return;     // sanity

    // Determine shr / mask from costume format.
    int shr, mask_bits;
    if (cost->num_colors == 32) { shr = 3; mask_bits = 0x07; }
    else                         { shr = 4; mask_bits = 0x0F; }

    // Position: actor base (dx, dy) plus xmove/ymove accumulator plus
    // this cel's relative offset (xmoveCur/ymoveCur). For a flipped
    // draw, the X offset is mirrored and we step columns leftward.
    int x_step = flip_x ? -1 : +1;
    int xmove_cur = xmove_pre + rx;
    int ymove_cur = ymove_pre + ry;
    int start_x = flip_x ? (dx - xmove_cur) : (dx + xmove_cur);
    int start_y = dy + ymove_cur;

    byle_rle_decode(src, res_end,
                    cw, ch,
                    start_x, start_y,
                    scale_x, scale_y,
                    x_step,
                    shr, mask_bits,
                    actor_palette, cost->palette,
                    mask_buf, mask_pitch, mask_x_off,
                    vscreen, vscreen_pitch,
                    VIRTUAL_SCREEN_W, VIRTUAL_SCREEN_H,
                    transparent_color);
}

// ---------------------------------------------------------------------------
// Animation engine — mirrors ClassicCostumeLoader::{costumeDecodeData,
// increaseAnim, increaseAnims} from scummvm-upstream/engines/scumm/
// costume.cpp:722-901.
// ---------------------------------------------------------------------------

// scummvm-upstream/engines/scumm/util.cpp:41-49.
int costume_new_dir_to_old(int dir) {
    if (dir >= 71 && dir <= 109)  return 1;       // E
    if (dir >= 109 && dir <= 251) return 2;       // S
    if (dir >= 251 && dir <= 289) return 0;       // W
    return 3;                                     // N
}

// Mirrors ClassicCostumeLoader::costumeDecodeData (costume.cpp:722-795).
// `frame` is the animation index (0..numAnim/4); `usemask` is the per-limb
// "should I touch this slot?" bitmask (0xFFFF = all). For each '1' bit in
// the usemask AND the per-anim mask read from the data stream, fetch the
// command-stream offset (j) and the extra byte; commands 0x79/0x7A
// toggle stopped, otherwise set up curpos/start/end/frame.
void costume_decode_data(Actor *a, int frame, unsigned usemask) {
    if (!a || a->costume == 0) return;
    Span cspan = resource_get_costume(a->costume);
    if (cspan.empty()) return;
    CostumeData cd{};
    if (!costume_parse(cspan, &cd) || !cd.valid) return;
    if (!cd.anim_cmds) return;

    int anim = costume_new_dir_to_old(a->facing) + frame * 4;
    if (anim > cd.num_anim) return;

    const uint8_t *res_end = cd.resource.data + cd.resource.size;
    if (cd.data_offsets + anim * 2 + 2 > res_end) return;

    const uint8_t *r = cd.baseptr + read_le16(cd.data_offsets + anim * 2);
    if (r == cd.baseptr) return;            // no data for this anim
    if (r >= res_end) return;

    // Per-anim limb mask: 16-bit little-endian for v2+
    // (`mask = READ_LE_UINT16(r)` at scummvm-upstream/engines/scumm/
    // costume.cpp:749). v1 packs only one byte (`mask = *r++ << 8`).
    // We target v4, so always LE16 here. (An earlier comment in this
    // file claimed BE — that was wrong, and led to limb-bit mismatches
    // that left animations like MI1's cloud actors stuck with all
    // limbs at 0xFFFF.)
    if (r + 2 > res_end) return;
    unsigned mask = (unsigned)read_le16(r);
    r += 2;

    int i = 0;
    do {
        if (mask & 0x8000) {
            // v4 path: 16-bit `j` (costume.cpp:763 "j = READ_LE_UINT16(r)").
            if (r + 2 > res_end) return;
            unsigned j = (unsigned)read_le16(r);
            r += 2;

            if (usemask & 0x8000) {
                if (j == 0xFFFF) {
                    a->cost.curpos[i] = 0xFFFF;
                    a->cost.start[i]  = 0;
                    a->cost.frame[i]  = (uint16_t)anim;
                } else {
                    if (r >= res_end) return;
                    uint8_t extra = *r++;
                    if ((cd.anim_cmds + j) >= res_end) return;
                    uint8_t cmd = cd.anim_cmds[j];
                    if (cmd == 0x7A) {
                        a->cost.stopped_mask &= ~(uint16_t)(1 << i);
                    } else if (cmd == 0x79) {
                        a->cost.stopped_mask |=  (uint16_t)(1 << i);
                    } else {
                        a->cost.curpos[i] = (uint16_t)j;
                        a->cost.start[i]  = (uint16_t)j;
                        a->cost.end[i]    = (uint16_t)(j + (extra & 0x7F));
                        if (extra & 0x80)
                            a->cost.curpos[i] |= 0x8000;
                        a->cost.frame[i] = (uint16_t)anim;
                    }
                }
            } else {
                // Skip the per-limb extra byte (j != 0xFFFF means there's
                // an extra). Mirrors costume.cpp:787-789.
                if (j != 0xFFFF) {
                    if (r >= res_end) return;
                    r++;
                }
            }
        }
        i++;
        usemask <<= 1;
        mask    <<= 1;
    } while (mask & 0xFFFF);
}

// Mirrors ClassicCostumeLoader::increaseAnim (costume.cpp:849-901). Walks
// the per-limb cur position forward through the anim_cmds stream until
// it hits a valid drawable command, looping start..end as needed and
// honouring the 0x8000 high bit (no-loop / clamp-at-end).
bool costume_increase_anim(Actor *a, int slot) {
    if (!a || a->costume == 0) return false;
    Span cspan = resource_get_costume(a->costume);
    if (cspan.empty()) return false;
    CostumeData cd{};
    if (!costume_parse(cspan, &cd) || !cd.valid) return false;
    if (!cd.anim_cmds) return false;

    if (a->cost.curpos[slot] == 0xFFFF) return false;

    int highflag = a->cost.curpos[slot] & 0x8000;
    int i        = a->cost.curpos[slot] & 0x7FFF;
    int end      = a->cost.end[slot];

    const uint8_t *res_end = cd.resource.data + cd.resource.size;
    auto cmd_at = [&](int idx) -> int {
        const uint8_t *p = cd.anim_cmds + idx;
        if (p < cd.resource.data || p >= res_end) return 0;
        return *p;
    };

    int code = cmd_at(i) & 0x7F;

    // For v<=3 sound counter — costume.cpp:862-864. v4 too. We don't
    // model actor sound counters yet; ignore.

    while (true) {
        if (!highflag) {
            // Loop start..end inclusive.
            if (i++ >= end) i = a->cost.start[slot];
        } else {
            // High flag: clamp at end.
            if (i != end) i++;
        }

        int nc = cmd_at(i);
        if (nc == 0x7C) {
            // animCounter++ (we don't model — fall through). If start ==
            // end this is an explicit "no-progress" marker.
            if (a->cost.start[slot] != end) continue;
        } else if (nc == 0x78) {
            // Sound counter (v<=5). Skip.
            if (a->cost.start[slot] != end) continue;
        }

        a->cost.curpos[slot] = (uint16_t)(i | highflag);
        return (cmd_at(i) & 0x7F) != code;
    }
}

bool costume_increase_anims(Actor *a) {
    bool r = false;
    for (int i = 0; i < 16; i++) {
        if (a->cost.curpos[i] != 0xFFFF) {
            if (costume_increase_anim(a, i)) r = true;
        }
    }
    return r;
}

}  // namespace tsb
