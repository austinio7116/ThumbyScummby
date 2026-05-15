// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — persistent save slot backend.
//
// Multi-slot save: kNumSlots independent slots.  Each slot owns its own
// flash region (device) or its own file (host SDL).  Per-slot metadata
// (thumbnail + hint string) is written by save_backend itself BEFORE
// the SCUMM payload — the engine's saveState/loadState see only the
// payload portion of the stream.
//
// Slot layout (in-region):
//   [0..15]       header { magic, version, payload_size, reserved }
//   [16..]        thumbnail — kThumbW * kThumbH RGB565 pixels, raw
//                 (kThumbBytes bytes; currently 4608 for 64×36)
//   [next]        hint — NUL-terminated, kHintBytes total
//   [next..]      SCUMM save payload (saveState output)

#pragma once

#include "types.h"

namespace Common {
class SeekableWriteStream;
class SeekableReadStream;
}

namespace tsb {
namespace save_backend {

// Slot count + thumbnail dimensions are exposed so the save-menu UI
// and the engine wrapper agree on layout without duplicating constants.
constexpr int kNumSlots  = 4;
constexpr int kThumbW    = 64;
constexpr int kThumbH    = 36;
constexpr int kThumbBytes = kThumbW * kThumbH * 2;   // 4608 bytes RGB565
constexpr int kHintBytes  = 48;                       // NUL-terminated ASCII

// One-line metadata for a slot, suitable for paint code that doesn't
// want to keep file handles open.  thumb is a const pointer into XIP
// (device) or into a static cache (host) — valid as long as no save
// is mid-write.
struct SlotInfo {
    bool occupied;
    char hint[kHintBytes];
    const uint16_t *thumb;   // kThumbW * kThumbH RGB565, or nullptr if empty
};

// Fill `out[0..kNumSlots-1]` with current slot state.  Backends that
// can return XIP-resident thumb pointers (flash backend) write them
// directly; backends without XIP-resident data (FatFs) allocate
// transient buffers on the heap — caller must pair enumerate_slots
// with release_slots before discarding the array.
void enumerate_slots(SlotInfo *out);

// Free any per-slot resources owned by `out` (e.g. heap-allocated
// thumbnail buffers in the FatFs backend).  Safe to call on a
// zero-initialised array.  No-op on backends with no transient
// allocations.
void release_slots(SlotInfo *out);

// Open a slot for writing.  Pre-writes header + thumbnail + hint; the
// stream returned is positioned past the metadata, so the engine's
// saveState writes only its own payload.  Caller deletes when done
// (deletion finalizes the header).  Pass thumb=nullptr to write a
// blanked thumbnail (used by tests).
Common::SeekableWriteStream *open_for_writing(int slot,
                                              const uint16_t *thumb,
                                              const char *hint);

// Open a slot for reading.  Stream is positioned past the metadata,
// so the engine's loadState sees only its own payload.  Returns nullptr
// if the slot is empty.
Common::SeekableReadStream *open_for_reading(int slot);

// True if slot has a valid save.
bool has_save(int slot);

}  // namespace save_backend
}  // namespace tsb
