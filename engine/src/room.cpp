// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
#include "room.h"
#include "small_chunk.h"
#include "smap.h"
#include "platform.h"

#include <string.h>

namespace tsb {

// ---------------------------------------------------------------------------
// Locate a room within a DISK0n.LEC file.
// LEC layout (small_header):
//   LECF wrapper { LE size + "LE" } encloses the entire file
//   LOFF { LE size + "FO" } maps room_id -> offset_within_lec
//   LFLF { LE size + "LF" } per room — wraps the actual ROOM small-chunk
//
// readRoomsOffsets() in ScummVM seeks to offset 12 (skips LECF+LOFF
// 6-byte headers) then reads LOFF entries directly.
// ---------------------------------------------------------------------------

bool room_load(int room_id, const MasterIndex &master, Room *out) {
    memset(out, 0, sizeof(*out));
    out->id = room_id;

    if (room_id < 1 || room_id >= master.num_rooms) {
        platform::log("room_load: id %d out of range\n", room_id);
        return false;
    }
    auto &entry = master.rooms[room_id];
    if (entry.disk == 0) {
        platform::log("room_load: room %d not on any disk\n", room_id);
        return false;
    }
    Span disk = platform::data_disk(entry.disk);
    if (disk.empty()) {
        platform::log("room_load: disk %d not loaded\n", entry.disk);
        return false;
    }

    // entry.offset points at the LFLF wrapper (small chunk: 6-byte header
    // then ROOM small-chunk inside).
    if (entry.offset + 6 > disk.size) {
        platform::log("room_load: room %d offset %u out of bounds\n",
                      room_id, entry.offset);
        return false;
    }

    SmallChunk lflf{};
    if (!small_read(disk.sub(entry.offset), &lflf)) {
        platform::log("room_load: room %d bad LFLF header at offset %u\n",
                      room_id, entry.offset);
        return false;
    }
    platform::log("room_load: room %d at disk %u offs 0x%X tag=0x%04X size=%u\n",
                  room_id, entry.disk, entry.offset, lflf.tag, lflf.size);
    // First few payload bytes for diagnostics
    platform::log("  payload[0..15]:");
    for (size_t i = 0; i < 16 && i < lflf.payload.size; i++)
        platform::log(" %02x", lflf.payload.data[i]);
    platform::log("\n");
    if (lflf.tag != stag::LF) {
        platform::log("room_load: room %d expected 'LF' got 0x%04X (LFLF wrapper)\n",
                      room_id, lflf.tag);
        // Some games store the ROOM directly; try anyway.
    }

    // LFLF payload starts with `uint16 LE room_id` then the ROOM small-chunk.
    Span body_after_id = lflf.payload.sub(2);
    SmallChunk room_chunk{};
    if (!small_read(body_after_id, &room_chunk) ||
        room_chunk.tag != stag::RO) {
        platform::log("room_load: room %d ROOM chunk missing/bad (tag=0x%04X)\n",
                      room_id, room_chunk.tag);
        return false;
    }
    out->room_chunk = room_chunk.payload;

    // Dump all sub-chunks for diagnostic
    {
        size_t cur = 0;
        SmallChunk c2{};
        platform::log("room %d sub-chunks:", room_id);
        while (small_next(room_chunk.payload, &cur, &c2)) {
            char a = (char)(c2.tag & 0xFF);
            char b = (char)((c2.tag >> 8) & 0xFF);
            platform::log(" %c%c(%u)", a, b, c2.size);
        }
        platform::log("\n");
    }

    // Find sub-chunks
    SmallChunk c{};
    if (small_find(room_chunk.payload, stag::HD, &c)) {
        // RMHD: uint16 width, height, numObjects (LE)
        if (c.payload.size >= 6) {
            out->rmhd_payload = c.payload;
            out->width        = read_le16(c.payload.data);
            out->height       = read_le16(c.payload.data + 2);
            out->num_objects  = read_le16(c.payload.data + 4);
        }
    }

    if (small_find(room_chunk.payload, stag::TR, &c)) {
        if (c.payload.size >= 1) out->transparent_color = c.payload.data[0];
    }

    if (small_find(room_chunk.payload, stag::PA, &c) ||
        small_find(room_chunk.payload, stag::SL, &c) ||
        small_find(room_chunk.payload, small_tag('C','L'), &c)) {
        out->palette_payload = c.payload;
    }

    if (small_find(room_chunk.payload, stag::BM, &c) ||
        small_find(room_chunk.payload, small_tag('R','M'), &c)) {
        out->rmim_payload = c.payload;
        // SMAP body inside (different tag in v4: 'SP' for strip data?)
        // Actually in v4, the room-image chunk is 'BM' and contains the
        // strips directly without an inner SMAP wrapper. We'll handle
        // both layouts in the SMAP decoder.
        out->bm_smap_payload = c.payload;
    }

    // Walkbox data: 'BX' = BOXD (vertices). The matrix ('BM' inside SCRP)
    // is normally absent in v4; we compute it ourselves at room load time.
    if (small_find(room_chunk.payload, stag::BX, &c)) {
        out->boxd_payload = c.payload;
    }

    // CYCL ('CC' in v4) — palette cycle table. Mirrors
    // ScummEngine::initCycl, GF_SMALL_HEADER branch (palette.cpp:605-620):
    // 16 entries, each (BE16 delay, u8 start, u8 end). counter is set to
    // `start` if delay != 0 && delay != 0x0AAA.
    memset(out->color_cycle, 0, sizeof(out->color_cycle));
    if (small_find(room_chunk.payload, small_tag('C','C'), &c) &&
        c.payload.size >= 16 * 4) {
        const uint8_t *p = c.payload.data;
        for (int i = 0; i < 16; i++, p += 4) {
            uint16_t delay = (uint16_t)((p[0] << 8) | p[1]);   // BE
            out->color_cycle[i].delay   = delay;
            out->color_cycle[i].start   = p[2];
            out->color_cycle[i].end     = p[3];
            out->color_cycle[i].counter = 0;
            if (delay && delay != 0x0AAA)
                out->color_cycle[i].counter = p[2];
        }
    }

    // Room entry / exit code (ENCD / EXCD). The offset is relative to the
    // ROOM resource base — we use room_chunk.full (which starts at the
    // 6-byte small-chunk header) so it matches ScummVM's roomResPtr from
    // getResourceAddress(rtRoom, X).
    out->room_resource = room_chunk.full;
    if (small_find(room_chunk.payload, stag::EN, &c)) {
        out->encd_payload = c.payload;
        out->encd_offset = (uint32_t)(c.payload.data - room_chunk.full.data);
    }
    if (small_find(room_chunk.payload, stag::EX, &c)) {
        out->excd_payload = c.payload;
        out->excd_offset = (uint32_t)(c.payload.data - room_chunk.full.data);
    }

    // Local scripts: walk every LS sibling. Each LSCR's body starts with a
    // 1-byte ID, then the bytecode (matches resource.cpp:459: ptr+1).
    for (int i = 0; i < Room::MAX_LOCAL_SCRIPTS; i++) {
        out->lscr_payload[i] = Span{nullptr, 0};
        out->lscr_offset[i] = 0;
    }
    {
        size_t cur2 = 0;
        SmallChunk lscr{};
        while (small_next(room_chunk.payload, &cur2, &lscr)) {
            if (lscr.tag != stag::LS) continue;
            if (lscr.payload.size < 2) continue;
            int id = lscr.payload.data[0];
            // Per resource.cpp:459: _localScriptOffsets[id - 200] = ptr + 1.
            int idx = id - 200;
            if (idx < 0 || idx >= Room::MAX_LOCAL_SCRIPTS) continue;
            out->lscr_payload[idx] = lscr.payload.sub(1);
            out->lscr_offset[idx] =
                (uint32_t)(lscr.payload.data + 1 - room_chunk.full.data);
        }
    }

    platform::log("room %d: %dx%d, %d objects, palette=%zu B, image=%zu B\n",
                  room_id, out->width, out->height, out->num_objects,
                  out->palette_payload.size, out->bm_smap_payload.size);
    return true;
}

bool room_load_palette(const Room &room, uint8_t *out_palette) {
    // Default palette = grayscale ramp (visible if real palette missing)
    for (int i = 0; i < 256; i++) {
        out_palette[i*3 + 0] = (uint8_t)i;
        out_palette[i*3 + 1] = (uint8_t)i;
        out_palette[i*3 + 2] = (uint8_t)i;
    }

    if (room.palette_payload.empty()) {
        platform::log("room: no palette data; using grayscale\n");
        return false;
    }

    // V4 small-header 'PA' chunk:
    //   uint16 LE   numcolor * 3       (size prefix)
    //   uint8[]     RGB triplets       (0..255 directly, NOT 6-bit)
    if (room.palette_payload.size < 2) return false;
    uint16_t size_field = read_le16(room.palette_payload.data);
    int n_colors = size_field / 3;
    if (n_colors > 256) n_colors = 256;
    if (room.palette_payload.size < (size_t)2 + (size_t)n_colors * 3) {
        platform::log("room: palette truncated (size_field=%u, payload=%zu)\n",
                      size_field, room.palette_payload.size);
        return false;
    }
    const uint8_t *p = room.palette_payload.data + 2;
    for (int i = 0; i < n_colors; i++) {
        out_palette[i*3 + 0] = p[i*3 + 0];
        out_palette[i*3 + 1] = p[i*3 + 1];
        out_palette[i*3 + 2] = p[i*3 + 2];
    }
    return true;
}

// ---------------------------------------------------------------------------
// Palette cycle tick. Mirrors the v4 (GF_SMALL_HEADER) branch of
// ScummEngine::cyclePalette (palette.cpp:741-768). Rotates the
// _shadowPalette[start..end] indirection table by one step per call;
// the next room->screen blit picks up the new mapping. The actual
// _currentPalette RGB stays fixed.
// ---------------------------------------------------------------------------
void palette_cycle_tick(ColorCycle cycles[16], uint8_t *shadow_palette) {
    for (int i = 0; i < 16; i++) {
        ColorCycle &c = cycles[i];
        if (!c.counter) continue;
        c.counter++;
        if (c.counter > c.end) c.counter = c.start;
        if (c.start > c.end) continue;

        // palette.cpp:754-760 verbatim algorithm.
        uint8_t cycle_val = (uint8_t)c.counter;
        for (int j = c.start; j <= c.end; j++) {
            shadow_palette[j] = cycle_val--;
            if (cycle_val < c.start) cycle_val = (uint8_t)c.end;
        }
    }
}

// ---------------------------------------------------------------------------
// Background render — Phase 2 stub. Real SMAP decoder lives in smap.cpp.
// ---------------------------------------------------------------------------
bool room_render_background(const Room &room, uint8_t *out_buf, int out_pitch) {
    // Clear screen
    for (int y = 0; y < VIRTUAL_SCREEN_H; y++) {
        memset(out_buf + y * out_pitch, 0, VIRTUAL_SCREEN_W);
    }
    if (room.bm_smap_payload.empty()) {
        platform::log("room: no image data\n");
        return false;
    }
    // The bm_smap_payload spans the entire BM chunk content. Decode it
    // into the output buffer at (0, 0).
    return smap_decode_bm(room.bm_smap_payload,
                          room.width, room.height,
                          out_buf, out_pitch);
}

}  // namespace tsb
