// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — RP2350 telemetry backend.
//
// Single 4 KB flash sector at offset (top - 68 KB), one record below
// the save region.  Each record is one 256-byte page; 16 records per
// sector, log-structured (program-only, erase the sector when full).
// On read, scan all 16 pages and pick the highest serial number that
// has a valid magic.

#include "telemetry.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <malloc.h>   // mallinfo on newlib for the actual heap state

namespace tsb {
namespace telemetry {

namespace {

// Sector layout — sits just below the save region (top 64 KB).
constexpr uint32_t kFlashRegionOffset = 16u * 1024u * 1024u - 68u * 1024u;  // 0x00FEF000
constexpr uint32_t kFlashRegionSize   = 4u * 1024u;                          // one sector
constexpr uintptr_t kFlashXipBase     = 0x10000000u;
constexpr uint32_t kRecordSize        = FLASH_PAGE_SIZE;                     // 256 B
constexpr uint32_t kRecordsPerSector  = kFlashRegionSize / kRecordSize;      // 16

constexpr uint32_t kRecordMagic   = 0x544D4253u;   // 'SBMT'
constexpr uint16_t kRecordVersion = 1;

// On-flash record layout — fixed at 256 bytes (one page).  Pad/reserve
// fields so future versions can grow without breaking parsing.
struct __attribute__((packed)) Record {
    uint32_t magic;            // 'SBMT'  — invalid record if mismatched
    uint16_t version;
    uint16_t reserved0;
    uint32_t serial;           // monotonic, higher wins on read
    uint32_t millis;           // wall ms at the time of the checkpoint
    int32_t  room;             // current room number
    uint32_t heap_used;        // resmgr _allocatedSize
    uint32_t heap_peak;        // newlib mallinfo uordblks (in-use)
    uint32_t newlib_free;      // newlib mallinfo fordblks (free bytes)
    uint32_t newlib_chunks;    // newlib mallinfo ordblks (free chunks)
    uint32_t alloc_size;       // size of last failed alloc (0 if none)
    uint8_t  alloc_kind;       // 'm'/'c'/'r' (or 0 if no failure)
    uint8_t  reserved_pad[3];  // align alloc_addr
    uint32_t alloc_addr;       // caller PC at the failed alloc
    char     event[40];        // null-terminated short tag
    uint8_t  reserved1[256 - 4 - 2 - 2 - 4 - 4 - 4 - 4 - 4 - 4 - 4 - 4 - 4 - 4 - 40];
};
static_assert(sizeof(Record) == kRecordSize, "telemetry record must be 256 B");

const uint8_t *xip_ptr(uint32_t flash_offs) {
    return reinterpret_cast<const uint8_t *>(kFlashXipBase + flash_offs);
}

const Record *record_ptr(uint32_t slot_idx) {
    return reinterpret_cast<const Record *>(
        xip_ptr(kFlashRegionOffset + slot_idx * kRecordSize));
}

// In-flight record, mutated by set_*() calls.  checkpoint() copies this
// into a 256-byte page and programs it.
Record g_live = {};
uint32_t g_serial_seed = 0;
bool g_initialized = false;

// Find the highest serial across all valid records.  Returns 0 if no
// valid record exists.  Used both at boot (to seed g_serial_seed) and
// for read_last().
const Record *find_latest() {
    const Record *best = nullptr;
    for (uint32_t i = 0; i < kRecordsPerSector; ++i) {
        const Record *r = record_ptr(i);
        if (r->magic != kRecordMagic) continue;
        if (r->version != kRecordVersion) continue;
        if (!best || r->serial > best->serial) best = r;
    }
    return best;
}

// Find the next slot to program.  Convention: slots are written in
// ascending order; first slot whose magic is 0xFFFFFFFF (i.e. erased)
// is the next write target.  If none — sector is full, erase + start
// at slot 0.
uint32_t next_slot() {
    for (uint32_t i = 0; i < kRecordsPerSector; ++i) {
        if (record_ptr(i)->magic == 0xFFFFFFFFu) return i;
    }
    return kRecordsPerSector;  // sentinel = full
}

void erase_sector() {
    uint32_t flags = save_and_disable_interrupts();
    flash_range_erase(kFlashRegionOffset, kFlashRegionSize);
    restore_interrupts(flags);
}

void program_record(uint32_t slot_idx, const Record &rec) {
    // Stage the record into a fixed-page buffer (flash_range_program
    // requires a 256-byte aligned source for one page).
    static uint8_t page[FLASH_PAGE_SIZE];
    std::memset(page, 0xFF, sizeof(page));
    std::memcpy(page, &rec, sizeof(rec));

    uint32_t flash_offs = kFlashRegionOffset + slot_idx * kRecordSize;
    uint32_t flags = save_and_disable_interrupts();
    flash_range_program(flash_offs, page, FLASH_PAGE_SIZE);
    restore_interrupts(flags);
}

void ensure_init() {
    if (g_initialized) return;
    g_initialized = true;
    g_live.magic = kRecordMagic;
    g_live.version = kRecordVersion;
    const Record *latest = find_latest();
    g_serial_seed = latest ? latest->serial : 0;
}

}  // anonymous

void set_heap(uint32_t used, uint32_t peak) {
    ensure_init();
    g_live.heap_used = used;
    g_live.heap_peak = peak;
}

void set_room(int room) {
    ensure_init();
    g_live.room = room;
}

void set_alloc(uint32_t size, uint8_t kind, uint32_t addr) {
    ensure_init();
    g_live.alloc_size = size;
    g_live.alloc_kind = kind;
    g_live.alloc_addr = addr;
}

void set_event(const char *tag) {
    ensure_init();
    if (!tag) { g_live.event[0] = 0; return; }
    int i = 0;
    for (; i < (int)sizeof(g_live.event) - 1 && tag[i]; ++i)
        g_live.event[i] = tag[i];
    g_live.event[i] = 0;
}

void checkpoint() {
    ensure_init();
    g_live.serial = ++g_serial_seed;
    g_live.millis = (uint32_t)to_ms_since_boot(get_absolute_time());

    // Auto-capture newlib's actual heap state on every checkpoint.
    // The engine's heap_used field is the resmgr's _allocatedSize
    // (logical resource bytes); heap_peak holds newlib's mallinfo
    // uordblks (in-use bytes for ALL malloc calls including non-
    // resmgr allocations like iMUSE buffers, screens, costume
    // renderer scratch).  The boot splash displays both so we can
    // see how much non-resmgr heap is in play and whether the next
    // alloc that crashes is hitting a real OOM ceiling.
    struct mallinfo mi = mallinfo();
    g_live.heap_peak     = (uint32_t)mi.uordblks;
    g_live.newlib_free   = (uint32_t)mi.fordblks;
    g_live.newlib_chunks = (uint32_t)mi.ordblks;

    uint32_t slot = next_slot();
    if (slot >= kRecordsPerSector) {
        erase_sector();
        slot = 0;
    }
    program_record(slot, g_live);
}

bool read_last(Snapshot *out) {
    if (!out) return false;
    const Record *latest = find_latest();
    if (!latest) return false;
    out->serial    = latest->serial;
    out->millis    = latest->millis;
    out->room      = latest->room;
    out->heap_used     = latest->heap_used;
    out->heap_peak     = latest->heap_peak;
    out->newlib_free   = latest->newlib_free;
    out->newlib_chunks = latest->newlib_chunks;
    out->alloc_size    = latest->alloc_size;
    out->alloc_kind    = latest->alloc_kind;
    out->alloc_addr    = latest->alloc_addr;
    std::memcpy(out->event, latest->event, sizeof(out->event));
    out->event[sizeof(out->event) - 1] = 0;
    return true;
}

void boot() {
    ensure_init();
    set_event("BOOT");
    checkpoint();
}

}  // namespace telemetry
}  // namespace tsb
