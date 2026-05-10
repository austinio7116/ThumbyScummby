// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — panic handler that telemetries OOMs.
//
// Wired via PICO_PANIC_FUNCTION (CMakeLists.txt).  The SDK's panic()
// becomes a naked function that jumps here then bkpt's.  We use the
// short window before the breakpoint to write the panic message into
// the flash telemetry record so the next-boot splash shows it.
//
// Combined with our oom_malloc.c wrappers, ANY malloc/calloc/realloc
// that returns NULL (or grows past __StackLimit) calls panic("Out of
// memory") AFTER stashing the requested size + entrypoint kind into
// thumby_last_alloc_size / _kind.  We forward both into the live
// telemetry record (set_alloc) so the next-boot splash shows them on
// their own line, e.g. "alloc m4096" — a 4 KB malloc that pushed us
// over.  Catches OOMs from iMUSE, OPL2, engine globals, anywhere —
// not just createResource.
//
// The handler must NOT itself panic (so don't malloc, don't recurse).
// telemetry::checkpoint() writes a fixed static page buffer to flash
// — no malloc.  Safe.

#include "telemetry.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>

extern "C" {
    extern volatile uint32_t thumby_last_alloc_size;
    extern volatile uint8_t  thumby_last_alloc_kind;
    extern volatile uint32_t thumby_last_alloc_addr;
}

extern "C" void thumby_panic_handler(const char *fmt, ...) {
    // Format the panic message into our telemetry tag (40 chars max).
    char tag[64];
    tag[0] = 'P'; tag[1] = ':'; tag[2] = ' ';
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(tag + 3, sizeof(tag) - 3, fmt, ap);
        va_end(ap);
    } else {
        std::strcpy(tag + 3, "(null)");
    }
    tsb::telemetry::set_event(tag);

    // Forward the captured failed-alloc size/kind into the live record
    // so the splash can render it on a dedicated line (no risk of the
    // size getting truncated off the end of the panic message).
    if (thumby_last_alloc_size) {
        tsb::telemetry::set_alloc(thumby_last_alloc_size,
                                  thumby_last_alloc_kind,
                                  thumby_last_alloc_addr);
    }

    // Persist BEFORE the SDK's bkpt halts the CPU.
    tsb::telemetry::checkpoint();

    // Return — the SDK's panic() epilogue (bkpt #0 + infinite loop)
    // halts execution next.
}
