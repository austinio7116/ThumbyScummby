// ThumbyScummby — 000.LFL master index parser (SCUMM v4 small_header).
//
// MI1 VGA Floppy 000.LFL layout:
//   each block = uint32 LE size + uint16 LE blocktype + payload
//   blocktype magics:
//     'NR' (0x4E52) - room names
//     'R0' (0x5230) - rooms       -> uint16 LE count + N*(uint8 disk + uint32 LE offs)
//     'S0' (0x5330) - scripts     -> same
//     'N0' (0x4E30) - sounds      -> same
//     'C0' (0x4330) - costumes    -> same
//     'O0' (0x4F30) - global objs -> uint16 LE count + N*(uint32 LE class+state+owner)

#pragma once

#include "types.h"

namespace tsb {

constexpr int MAX_ROOMS    = 256;
constexpr int MAX_SCRIPTS  = 512;
constexpr int MAX_SOUNDS   = 256;
constexpr int MAX_COSTUMES = 256;
constexpr int MAX_GLOBAL_OBJECTS = 1024;

// One resource directory entry: which disk file holds it, where in that file.
struct ResourceEntry {
    uint8_t  disk;        // 0..4 (0 = none / not present)
    uint32_t offset;      // byte offset into DISK0n.LEC
};

// Global object metadata (state + owner + class).
struct GlobalObject {
    uint32_t classes_state_owner;  // packed: top byte=state, next=owner, low 24=class bits
};

struct MasterIndex {
    int  num_rooms;
    int  num_scripts;
    int  num_sounds;
    int  num_costumes;
    int  num_global_objects;

    ResourceEntry  rooms[MAX_ROOMS];
    ResourceEntry  scripts[MAX_SCRIPTS];
    ResourceEntry  sounds[MAX_SOUNDS];
    ResourceEntry  costumes[MAX_COSTUMES];
    GlobalObject   global_objects[MAX_GLOBAL_OBJECTS];
};

// Parse 000.LFL contents into the index struct. Returns false on format error.
bool parse_master_index(Span data, MasterIndex *out);

// Walk each loaded disk's LOFF table and fill in per-room offsets.
// 000.LFL only tells us which disk each room lives on; the actual offset
// within DISK0n.LEC comes from that disk's LOFF table.
void resolve_room_offsets(MasterIndex *index);

}  // namespace tsb
