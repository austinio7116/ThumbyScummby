// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — host telemetry stub.  No flash on host; we just
// echo set_event() / checkpoint() to stderr if THUMBY_TELEMETRY env
// is set, and read_last() always returns false.

#include "telemetry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tsb {
namespace telemetry {

namespace {
struct Live {
    int      room       = 0;
    uint32_t heap_used  = 0;
    uint32_t heap_peak  = 0;
    char     event[40]  = {0};
    uint32_t serial     = 0;
};
Live g_live;
}  // anonymous

void set_heap(uint32_t used, uint32_t peak) {
    g_live.heap_used = used;
    g_live.heap_peak = peak;
}

void set_room(int room) {
    g_live.room = room;
}

void set_event(const char *tag) {
    if (!tag) { g_live.event[0] = 0; return; }
    int i = 0;
    for (; i < (int)sizeof(g_live.event) - 1 && tag[i]; ++i)
        g_live.event[i] = tag[i];
    g_live.event[i] = 0;
}

void checkpoint() {
    ++g_live.serial;
    if (getenv("THUMBY_TELEMETRY")) {
        fprintf(stderr,
            "[telemetry #%u] room=%d heap=%u peak=%u event='%s'\n",
            (unsigned)g_live.serial, g_live.room,
            (unsigned)g_live.heap_used, (unsigned)g_live.heap_peak,
            g_live.event);
    }
}

bool read_last(Snapshot * /*out*/) {
    return false;
}

void boot() {
    set_event("BOOT");
    checkpoint();
}

}  // namespace telemetry
}  // namespace tsb
