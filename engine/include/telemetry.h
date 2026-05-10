// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — flash-backed telemetry for diagnosing device hangs.
//
// Cannot use USB serial on the Thumby Color, so freeze/hang root-cause
// has to come from a side-channel.  This module persists the "last
// known state" to a single flash sector below the save region, and on
// next boot the splash screen surfaces it for ~3 s so we can read what
// the engine was doing when it went down.
//
// Event budget: ~50 ms per checkpoint (sector erase + page program), so
// only call from non-hot paths — room transitions, heap-purge entry/exit.

#pragma once

#include "types.h"

namespace tsb {
namespace telemetry {

// ----- Live state (cheap RAM-only) -----

// Update fields of the in-flight record without touching flash.  Use
// for fast continuous updates (e.g. per scummLoop tick): millis_now
// and heap stats.  Pass 0 / null to skip a field.
void set_heap(uint32_t used, uint32_t peak);
void set_room(int room);
void set_event(const char *tag);   // copied; up to 39 chars

// Record the size + entrypoint kind + return-address of the most
// recent failed allocation, captured by oom_malloc.c right before
// panic().  'kind' is 'm' (malloc), 'c' (calloc), or 'r' (realloc);
// 'addr' is the caller's PC.  Persisted on the next checkpoint so
// the next-boot splash shows what failed and where.
void set_alloc(uint32_t size, uint8_t kind, uint32_t addr);

// ----- Checkpoint (slow ~50 ms) -----

// Commit the current in-flight state to flash, in a single page.  Each
// checkpoint advances the serial number; on read, the highest serial
// wins.  Sector auto-erases when full (every 16 checkpoints).
//
// Skips if there is no live state OR if the heap-trace mode is disabled
// at compile time.  Caller should bracket noisy events:
//
//   telemetry::set_event("ENTER 53"); telemetry::checkpoint();
//   ... room load ...
//   telemetry::set_event("LOADED 53"); telemetry::checkpoint();
void checkpoint();

// ----- Boot read -----

struct Snapshot {
    uint32_t serial;        // monotonic counter across boots/checkpoints
    uint32_t millis;        // millis() at the time of the checkpoint
    int      room;
    uint32_t heap_used;     // resmgr _allocatedSize
    uint32_t heap_peak;     // newlib mallinfo uordblks (in-use bytes)
    uint32_t newlib_free;   // newlib mallinfo fordblks (free bytes)
    uint32_t newlib_chunks; // newlib mallinfo ordblks (count of free chunks)
    uint32_t alloc_size;    // size of last failed alloc (0 if none)
    uint8_t  alloc_kind;    // 'm', 'c', 'r' (or 0 if no failure)
    uint32_t alloc_addr;    // caller PC at the failed alloc (LR snapshot)
    char     event[40];
};

// Read the most recent persisted snapshot.  Returns false if no valid
// record exists yet (first boot ever, or sector erased).
bool read_last(Snapshot *out);

// Increment the boot counter; call once at startup before the first
// checkpoint.  Used to distinguish freezes (boot N → boot N+1 with no
// graceful tick) from clean exits.
void boot();

}  // namespace telemetry
}  // namespace tsb
