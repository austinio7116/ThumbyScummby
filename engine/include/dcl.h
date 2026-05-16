// PKWARE Data Compression Library (DCL "explode") decoder.
// Ported from ScummVM common/compression/dcl.cpp — algorithm is
// public, used by every LucasArts SCUMM/-era installer (PCV/LFG!
// archives in MI1, MI2, Indy4, Atlantis, etc).
//
// Streaming interface: caller supplies a `read_byte` callback that
// pulls bytes from anywhere (a multi-file PCV chain in our case).
// Caller supplies a `write_byte` callback that consumes decompressed
// output one byte at a time (we use this to drive cluster-by-cluster
// in-place install on FAT).
//
// Decoder owns a 4 KB dictionary buffer — pass one in via init() so
// the caller controls allocation (heap during install, freed at
// finish; never in BSS).

#pragma once

#include "types.h"

namespace tsb {

class DclDecoder {
public:
    // Source byte pump.  Return value < 0 means end-of-stream.
    typedef int (*ReadByteFn)(void *userdata);
    // Output byte sink.  Return false to abort decoding (caller
    // detected a write error or wants to stop).
    typedef bool (*WriteByteFn)(uint8_t byte, void *userdata);

    // Decode a DCL stream end-to-end.
    //   - `read_byte` / `write_byte` are the streaming callbacks
    //   - `dictionary` is a caller-owned 4096-byte buffer
    //   - `read_user` and `write_user` are passed back to the callbacks
    //   - `expected_size` (if non-zero) is the unpacked size promised
    //     by the FILE chunk header; decoding stops cleanly when that
    //     many bytes have been written
    // Returns true on success (end-of-stream token reached or
    // `expected_size` bytes produced).
    static bool decode(ReadByteFn read_byte, void *read_user,
                       WriteByteFn write_byte, void *write_user,
                       uint8_t *dictionary,
                       uint32_t expected_size);
};

}  // namespace tsb
