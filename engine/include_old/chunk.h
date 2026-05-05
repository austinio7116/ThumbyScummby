// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — SCUMM v5 chunk reader.
//
// Chunks: 4-byte big-endian tag + 4-byte big-endian inclusive size + payload.
// All ScummVM-format files (.LFL, .LEC) are sequences of chunks, often
// nested. This module provides search and traversal over a Span without any
// heap allocation.

#pragma once

#include "types.h"

namespace tsb {

// Make a 32-bit tag from a 4-char string at compile time.
constexpr uint32_t make_tag(char a, char b, char c, char d) {
    return ((uint32_t)(uint8_t)a << 24) | ((uint32_t)(uint8_t)b << 16) |
           ((uint32_t)(uint8_t)c <<  8) | (uint32_t)(uint8_t)d;
}

// Common SCUMM v5 tags
namespace tag {
    constexpr uint32_t LECF  = make_tag('L','E','C','F');
    constexpr uint32_t LOFF  = make_tag('L','O','F','F');
    constexpr uint32_t LFLF  = make_tag('L','F','L','F');
    constexpr uint32_t ROOM  = make_tag('R','O','O','M');
    constexpr uint32_t RMHD  = make_tag('R','M','H','D');
    constexpr uint32_t TRNS  = make_tag('T','R','N','S');
    constexpr uint32_t EPAL  = make_tag('E','P','A','L');
    constexpr uint32_t CLUT  = make_tag('C','L','U','T');
    constexpr uint32_t PALS  = make_tag('P','A','L','S');
    constexpr uint32_t CYCL  = make_tag('C','Y','C','L');
    constexpr uint32_t BOXD  = make_tag('B','O','X','D');
    constexpr uint32_t BOXM  = make_tag('B','O','X','M');
    constexpr uint32_t SCAL  = make_tag('S','C','A','L');
    constexpr uint32_t RMIM  = make_tag('R','M','I','M');
    constexpr uint32_t RMIH  = make_tag('R','M','I','H');
    constexpr uint32_t IM00  = make_tag('I','M','0','0');
    constexpr uint32_t SMAP  = make_tag('S','M','A','P');
    constexpr uint32_t ZP01  = make_tag('Z','P','0','1');
    constexpr uint32_t ZP02  = make_tag('Z','P','0','2');
    constexpr uint32_t ZP03  = make_tag('Z','P','0','3');
    constexpr uint32_t ZP04  = make_tag('Z','P','0','4');
    constexpr uint32_t OBIM  = make_tag('O','B','I','M');
    constexpr uint32_t IMHD  = make_tag('I','M','H','D');
    constexpr uint32_t OBCD  = make_tag('O','B','C','D');
    constexpr uint32_t CDHD  = make_tag('C','D','H','D');
    constexpr uint32_t VERB  = make_tag('V','E','R','B');
    constexpr uint32_t OBNA  = make_tag('O','B','N','A');
    constexpr uint32_t EXCD  = make_tag('E','X','C','D');
    constexpr uint32_t ENCD  = make_tag('E','N','C','D');
    constexpr uint32_t NLSC  = make_tag('N','L','S','C');
    constexpr uint32_t LSCR  = make_tag('L','S','C','R');
    constexpr uint32_t SCRP  = make_tag('S','C','R','P');
    constexpr uint32_t COST  = make_tag('C','O','S','T');
    constexpr uint32_t SOUN  = make_tag('S','O','U','N');
    constexpr uint32_t CHAR  = make_tag('C','H','A','R');

    // 000.LFL master directory tags
    constexpr uint32_t DOBJ  = make_tag('D','O','B','J');
    constexpr uint32_t DROO  = make_tag('D','R','O','O');  // sometimes 'DROOM' (5 chars in some specs)
    constexpr uint32_t DROOM = make_tag('D','R','O','O');  // same; chunk uses 4-char only
    constexpr uint32_t DSCR  = make_tag('D','S','C','R');
    constexpr uint32_t DCOS  = make_tag('D','C','O','S');
    constexpr uint32_t DSOU  = make_tag('D','S','O','U');
    constexpr uint32_t DCHR  = make_tag('D','C','H','R');
    constexpr uint32_t MAXS  = make_tag('M','A','X','S');
    constexpr uint32_t LFLF_ = make_tag('L','F','L','F');
}

// A chunk view — the tag + the inclusive size + the payload span.
struct Chunk {
    uint32_t tag;    // 4-byte BE tag
    Span     full;   // includes the 8-byte header
    Span     payload;// after the 8-byte header

    bool ok() const { return !full.empty() && payload.size + 8 <= full.size; }
};

// Read a chunk header at offset 0 of `s`. If valid, set out.tag/.payload.
// Returns false if the span is too small or size exceeds bounds.
bool chunk_read(Span s, Chunk *out);

// Find the first sub-chunk matching `tag` inside `parent_payload`. Search
// is non-recursive (immediate children only). Returns false if not found.
bool chunk_find(Span parent_payload, uint32_t tag, Chunk *out);

// Iterate sub-chunks. Initialize `cursor = 0`, then call repeatedly until
// it returns false. After each call, `out` describes the chunk and
// `cursor` is advanced past it.
bool chunk_next(Span parent_payload, size_t *cursor, Chunk *out);

}  // namespace tsb
