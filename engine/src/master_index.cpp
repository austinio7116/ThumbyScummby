// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
#include "master_index.h"
#include "platform.h"

#include <string.h>

namespace tsb {

// blocktype magics (uint16 LE)
constexpr uint16_t BT_NR = 0x4E52;  // room names
constexpr uint16_t BT_R0 = 0x5230;  // rooms
constexpr uint16_t BT_S0 = 0x5330;  // scripts
constexpr uint16_t BT_N0 = 0x4E30;  // sounds
constexpr uint16_t BT_C0 = 0x4330;  // costumes
constexpr uint16_t BT_O0 = 0x4F30;  // global objects

// Read a directory list following an R0/S0/N0/C0 block header.
// Format: uint16 LE count, then count * (uint8 disk + uint32 LE offset).
static bool read_dir(Span s, ResourceEntry *out, int max, int *out_count) {
    if (s.size < 2) return false;
    uint16_t n = read_le16(s.data);
    if (n > max) {
        platform::log("master: directory count %u exceeds max %d\n", n, max);
        return false;
    }
    if (s.size < (size_t)2 + (size_t)n * 5) return false;
    *out_count = n;
    const uint8_t *p = s.data + 2;
    for (int i = 0; i < n; i++) {
        out[i].disk   = p[0];
        out[i].offset = read_le32(p + 1);
        p += 5;
    }
    return true;
}

bool parse_master_index(Span data, MasterIndex *out) {
    memset(out, 0, sizeof(*out));

    // Two-pass parse — first read NR/R0/S0/N0/C0/O0 sizes, then the data.
    // For our purposes one pass is enough: each block is self-contained
    // because blocksize tells us how much to skip. We just walk and dispatch.
    size_t cursor = 0;
    while (cursor + 6 <= data.size) {
        uint32_t blocksize = read_le32(data.data + cursor);
        uint16_t blocktype = read_le16(data.data + cursor + 4);
        if (blocksize < 6 || cursor + blocksize > data.size) {
            platform::log("master: bad block at %zu (size=%u, file=%zu)\n",
                          cursor, blocksize, data.size);
            return false;
        }
        Span body = data.sub(cursor + 6, blocksize - 6);
        switch (blocktype) {
        case BT_NR:
            // 'NR' carries the per-room name strings the original
            // SCUMM tools embed for debugging. ScummVM only uses them
            // for `--debugflags=RESOURCE` traces; nothing in the
            // runtime engine reads them. Skip the body — the cursor
            // advance below moves past the chunk.
            break;
        case BT_R0:
            if (!read_dir(body, out->rooms, MAX_ROOMS, &out->num_rooms)) return false;
            break;
        case BT_S0:
            if (!read_dir(body, out->scripts, MAX_SCRIPTS, &out->num_scripts)) return false;
            break;
        case BT_N0:
            if (!read_dir(body, out->sounds, MAX_SOUNDS, &out->num_sounds)) return false;
            break;
        case BT_C0:
            if (!read_dir(body, out->costumes, MAX_COSTUMES, &out->num_costumes)) return false;
            break;
        case BT_O0: {
            // Global objects: uint16 LE count + count * (uint32 LE)
            if (body.size < 2) return false;
            uint16_t n = read_le16(body.data);
            if (n > MAX_GLOBAL_OBJECTS) {
                platform::log("master: too many globals: %u\n", n);
                return false;
            }
            out->num_global_objects = n;
            const uint8_t *p = body.data + 2;
            for (int i = 0; i < n && (p + 4) <= body.data + body.size; i++) {
                out->global_objects[i].classes_state_owner = read_le32(p);
                p += 4;
            }
            break;
        }
        default:
            platform::log("master: unknown block 0x%04X size=%u at %zu (skipping)\n",
                          blocktype, blocksize, cursor);
            break;
        }
        cursor += blocksize;
    }

    platform::log("master index: %d rooms, %d scripts, %d sounds, %d costumes, %d globals\n",
        out->num_rooms, out->num_scripts, out->num_sounds, out->num_costumes,
        out->num_global_objects);

    // Dump first few scripts for diagnostic
    for (int i = 1; i <= 5 && i < out->num_scripts; i++) {
        platform::log("  script %d -> room %u offset 0x%X\n",
                      i, out->scripts[i].disk, out->scripts[i].offset);
    }

    // Dump first few present rooms for sanity
    int dumped = 0;
    for (int i = 1; i < out->num_rooms && dumped < 10; i++) {
        if (out->rooms[i].disk != 0) {
            platform::log("  room %d -> disk %u offset 0x%08X\n",
                i, out->rooms[i].disk, out->rooms[i].offset);
            dumped++;
        }
    }
    return true;
}

// Walk LOFF tables from each disk to fill in room offsets.
// LEC layout (small_header):
//   bytes 0-5:  LECF small-chunk header (4-byte LE size + 'LE')
//   bytes 6-11: LOFF small-chunk header (4-byte LE size + 'FO')
//   byte 12:    uint8 num_rooms
//   per room:   uint8 room_id, uint32 LE offset_in_file
void resolve_room_offsets(MasterIndex *index) {
    for (int disk = 1; disk <= 4; disk++) {
        Span d = platform::data_disk(disk);
        if (d.empty()) continue;
        if (d.size < 13) continue;
        // Optional sanity-check the LECF / LOFF tags
        if (read_le16(d.data + 4) != 0x454C ||  // "LE"
            read_le16(d.data + 10) != 0x4F46) { // "FO"
            platform::log("disk %d: unexpected header tags\n", disk);
            continue;
        }
        uint8_t num = d.data[12];
        const uint8_t *p = d.data + 13;
        const uint8_t *end = d.data + d.size;
        int filled = 0;
        for (int i = 0; i < num && p + 5 <= end; i++) {
            uint8_t  rid  = p[0];
            uint32_t offs = read_le32(p + 1);
            if (rid < MAX_ROOMS && index->rooms[rid].disk == (uint8_t)disk) {
                index->rooms[rid].offset = offs;
                filled++;
            }
            p += 5;
        }
        platform::log("disk %d: LOFF has %u rooms (filled %d on this disk)\n",
                      disk, num, filled);
    }
}

}  // namespace tsb
