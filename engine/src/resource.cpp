// Resource lookup: turn (type, id) into a Span pointing into XIP data.
//
// For SCUMM v4 small_header layout, scripts/costumes/sounds live INSIDE
// the LFLF wrapper of their owning room. The master index gives us
// (disk, offset_within_room_LFLF) for each resource. We walk the LFLF to
// find the right small-chunk.

#include "resource.h"
#include "master_index.h"
#include "small_chunk.h"
#include "platform.h"

#include <string.h>

namespace tsb {

extern MasterIndex *resource_get_master_index();   // engine.cpp provides this

// For SCUMM v4 small-header non-room resources (scripts, costumes, sounds),
// the master index stores:
//   _roomno  -> the owning ROOM NUMBER (NOT disk number)
//   _roomoffs -> offset relative to that room's LFLF chunk start in the LEC
// The actual disk and LEC absolute offset come from the OWNING ROOM's entry.
static Span lookup_in_room_lflf(const ResourceEntry &entry, uint16_t tag,
                                int resource_id) {
    int owning_room = entry.disk;   // misnamed: actually room number for non-room resources
    if (owning_room == 0) return Span{nullptr, 0};

    MasterIndex *m = resource_get_master_index();
    if (!m) return Span{nullptr, 0};
    if (owning_room >= m->num_rooms) return Span{nullptr, 0};

    auto &room = m->rooms[owning_room];
    if (room.disk == 0) return Span{nullptr, 0};
    Span disk_data = platform::data_disk(room.disk);
    if (disk_data.empty()) return Span{nullptr, 0};

    // Layout: room.offset points at the LFLF small-chunk (6-byte header).
    // Inside LFLF body: 2-byte room_id, then ROOM small-chunk, then other
    // resources (SCRP, COST, SOUN). The offset stored in the master index
    // for these resources is relative to the END of the LFLF header + the
    // 2-byte room_id, i.e. the same starting point as the ROOM chunk.
    uint32_t abs_off = room.offset + 6 + 2 + entry.offset;
    if (abs_off + 6 > disk_data.size) {
        platform::log("resource: id %d offs 0x%X out of bounds (room=%d disk=%u room_offs=0x%X scr_offs=0x%X)\n",
                      resource_id, abs_off, owning_room, room.disk,
                      room.offset, entry.offset);
        return Span{nullptr, 0};
    }

    SmallChunk c{};
    if (!small_read(disk_data.sub(abs_off), &c)) {
        platform::log("resource: id %d small_read failed at 0x%X\n",
                      resource_id, abs_off);
        return Span{nullptr, 0};
    }
    if (c.tag != tag) {
        // Dump the bytes at that offset to debug
        char ta = (char)(c.tag & 0xFF), tb = (char)((c.tag >> 8) & 0xFF);
        char wa = (char)(tag & 0xFF), wb = (char)((tag >> 8) & 0xFF);
        platform::log("resource: id %d tag mismatch at 0x%X (room %d disk %u room_offs 0x%X scr_offs 0x%X): got '%c%c'(0x%04X) size=%u want '%c%c'(0x%04X)\n",
                      resource_id, abs_off, owning_room, room.disk,
                      room.offset, entry.offset,
                      ta, tb, c.tag, c.size, wa, wb, tag);
        return Span{nullptr, 0};
    }
    return c.payload;
}

void resource_init() {
    // No-op for now (XIP-only model).
}

Span resource_get_script(int script_id) {
    MasterIndex *m = resource_get_master_index();
    if (!m) return Span{nullptr, 0};
    if (script_id < 0 || script_id >= m->num_scripts)
        return Span{nullptr, 0};
    // SCRP small chunk has tag "SC" (low byte 'S' high byte 'C')
    return lookup_in_room_lflf(m->scripts[script_id], stag::SC, script_id);
}

Span resource_get_costume(int costume_id) {
    MasterIndex *m = resource_get_master_index();
    if (!m || costume_id < 0 || costume_id >= m->num_costumes)
        return Span{nullptr, 0};
    return lookup_in_room_lflf(m->costumes[costume_id], stag::CO, costume_id);
}

Span resource_get_sound(int sound_id) {
    MasterIndex *m = resource_get_master_index();
    if (!m || sound_id < 0 || sound_id >= m->num_sounds)
        return Span{nullptr, 0};
    return lookup_in_room_lflf(m->sounds[sound_id], stag::SO, sound_id);
}

Span resource_get_room(int room_id) {
    MasterIndex *m = resource_get_master_index();
    if (!m || room_id < 0 || room_id >= m->num_rooms)
        return Span{nullptr, 0};
    auto &e = m->rooms[room_id];
    if (e.disk == 0) return Span{nullptr, 0};
    Span disk = platform::data_disk(e.disk);
    if (disk.empty()) return Span{nullptr, 0};
    if (e.offset + 6 > disk.size) return Span{nullptr, 0};
    SmallChunk lflf{};
    if (!small_read(disk.sub(e.offset), &lflf)) return Span{nullptr, 0};
    return lflf.full;  // Returns the full LFLF chunk (caller drills in)
}

bool resource_load(int type, int id) {
    Span s;
    switch (type) {
        case 1: s = resource_get_script(id);  break;
        case 2: s = resource_get_sound(id);   break;
        case 3: s = resource_get_costume(id); break;
        case 4: s = resource_get_room(id);    break;
        default: return false;
    }
    return !s.empty();
}

}  // namespace tsb
