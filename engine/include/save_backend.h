// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — persistent save slot backend.
//
// Single-slot save: one stream open at a time.  Host (SDL): file-backed.
// Device (RP2350): flash-region backed (top 64 KB of flash, 16 sectors).
// The streams returned implement Common::SeekableWriteStream /
// Common::SeekableReadStream so saveState / loadState in saveload.cpp use
// them transparently.

#pragma once

#include "types.h"

namespace Common {
class SeekableWriteStream;
class SeekableReadStream;
}

namespace tsb {
namespace save_backend {

// Open the save slot for writing.  Caller deletes the returned stream
// when done; deletion finalizes (commits any pending page on device).
// Returns nullptr if the backend cannot allocate / erase.
Common::SeekableWriteStream *open_for_writing();

// Open the save slot for reading.  Returns nullptr if no save exists or
// the header is invalid.  Caller deletes when done.
Common::SeekableReadStream *open_for_reading();

// True if a valid save exists.  Used to gate the "LOAD" menu option.
bool has_save();

}  // namespace save_backend
}  // namespace tsb
