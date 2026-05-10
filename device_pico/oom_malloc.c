// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — malloc wrappers that capture the failed allocation
// size before panicking.
//
// Replaces the SDK's pico_malloc (skipped via SKIP_PICO_MALLOC in
// CMakeLists.txt).  Same wrap semantics as the SDK version, plus:
// when an alloc returns NULL or grows past __StackLimit, we stash the
// size + which entrypoint into globals that panic_handler.cpp reads,
// then call panic("Out of memory").  Next-boot splash shows "P: OOM
// malloc N" so we know exactly how big the failing alloc was.
//
// Must remain dependency-free: this runs from inside libc malloc, no
// recursion-safe state available.  Three plain word-sized writes only.

#include <stdlib.h>
#include "pico.h"
#include "pico/malloc.h"

extern void *REAL_FUNC(malloc)(size_t size);
extern void *REAL_FUNC(calloc)(size_t count, size_t size);
extern void *REAL_FUNC(realloc)(void *mem, size_t size);
extern void  REAL_FUNC(free)(void *mem);

extern char __StackLimit;  // linker symbol, top of usable heap

// Read by panic_handler.cpp to enrich the "Out of memory" message.
// 'kind' is one byte: 'm'=malloc, 'c'=calloc, 'r'=realloc.
// 'addr' is the LR captured at the wrapper entry — i.e. the
// instruction in the CALLER's code that issued the failing alloc.
// addr2line on firmware_thumbyscummby.elf resolves it to a file:line.
volatile uint32_t thumby_last_alloc_size = 0;
volatile uint8_t  thumby_last_alloc_kind = 0;
volatile uint32_t thumby_last_alloc_addr = 0;

// Force-noinline so __builtin_return_address(0) inside the wrappers
// returns the wrapper's caller (the actual code that issued malloc),
// not whoever called the wrapper from inside another inlined frame.
static __attribute__((noinline)) void check_alloc(
    void *mem, uint32_t size, char kind, uint32_t caller)
{
    if (!mem || (((char *)mem) + size) > &__StackLimit) {
        thumby_last_alloc_size = size;
        thumby_last_alloc_kind = (uint8_t)kind;
        thumby_last_alloc_addr = caller;
        panic("Out of memory");
    }
}

void *WRAPPER_FUNC(malloc)(size_t size) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    void *rc = REAL_FUNC(malloc)(size);
    check_alloc(rc, (uint32_t)size, 'm', lr);
    return rc;
}

void *WRAPPER_FUNC(calloc)(size_t count, size_t size) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    void *rc = REAL_FUNC(calloc)(count, size);
    check_alloc(rc, (uint32_t)(count * size), 'c', lr);
    return rc;
}

void *WRAPPER_FUNC(realloc)(void *mem, size_t size) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    void *rc = REAL_FUNC(realloc)(mem, size);
    check_alloc(rc, (uint32_t)size, 'r', lr);
    return rc;
}

void WRAPPER_FUNC(free)(void *mem) {
    REAL_FUNC(free)(mem);
}
