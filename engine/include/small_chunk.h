// SCUMM v4 small-header chunk reader.
// Format: 4-byte LE size (inclusive) + 2-byte ASCII tag + payload.

#pragma once

#include "types.h"

namespace tsb {

// 2-char tag packed as uint16. e.g. small_tag('L','E') == 0x454C.
constexpr uint16_t small_tag(char a, char b) {
    return (uint16_t)((uint8_t)b << 8) | (uint16_t)(uint8_t)a;
}

// Common small tags (the leading 2 chars of the corresponding 4-char names)
namespace stag {
    constexpr uint16_t LE = small_tag('L','E');  // LECF wrapper
    constexpr uint16_t FO = small_tag('F','O');  // LOFF
    constexpr uint16_t LF = small_tag('L','F');  // LFLF
    constexpr uint16_t RO = small_tag('R','O');  // ROOM
    constexpr uint16_t HD = small_tag('H','D');  // RMHD
    constexpr uint16_t BX = small_tag('B','X');  // BOXD
    constexpr uint16_t PA = small_tag('P','A');  // PALS / PA-something
    constexpr uint16_t SL = small_tag('S','L');  // PALS variant
    constexpr uint16_t BM = small_tag('B','M');  // RMIM/SMAP variant
    constexpr uint16_t SP = small_tag('S','P');  // SMAP / SPECIAL
    constexpr uint16_t OI = small_tag('O','I');  // OBIM
    constexpr uint16_t OC = small_tag('O','C');  // OBCD
    constexpr uint16_t EX = small_tag('E','X');  // EXCD
    constexpr uint16_t EN = small_tag('E','N');  // ENCD
    constexpr uint16_t LS = small_tag('L','S');  // LSCR
    constexpr uint16_t SC = small_tag('S','C');  // SCRP
    constexpr uint16_t CO = small_tag('C','O');  // COST
    constexpr uint16_t SO = small_tag('S','O');  // SOUN
    constexpr uint16_t NL = small_tag('N','L');  // NLSC
    constexpr uint16_t CC = small_tag('C','C');  // CYCL
    constexpr uint16_t EP = small_tag('E','P');  // EPAL
    constexpr uint16_t SA = small_tag('S','A');  // SCAL
    constexpr uint16_t TR = small_tag('T','R');  // TRNS
}

struct SmallChunk {
    uint16_t tag;     // 2-byte ASCII tag (uint16 LE-decoded)
    uint32_t size;    // total inclusive size in bytes
    Span     full;    // bytes 0..size-1
    Span     payload; // bytes 6..size-1

    bool ok() const { return size >= 6; }
};

// Read a small chunk header at offset 0 of s. Returns false if invalid or
// truncated.
bool small_read(Span s, SmallChunk *out);

// Find the first immediate-child small chunk with `tag` inside `parent_payload`.
bool small_find(Span parent_payload, uint16_t tag, SmallChunk *out);

// Iterate small sub-chunks. cursor starts at 0; stops returning false at end.
bool small_next(Span parent_payload, size_t *cursor, SmallChunk *out);

}  // namespace tsb
