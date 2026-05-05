// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — scummvm-upstream compatibility shim.
//
// This header lets us *transcribe* canonical scummvm source files into the
// ThumbyScummby engine with the smallest possible delta — ideally a header
// include, a namespace wrapper, and `#if 0` blocks around versions/games we
// don't ship. Anything more invasive than that is a code smell.
//
// Strategy:
//   1. Type aliases: byte / uint16 / int16 / etc. (matching scummvm-upstream's
//      common/scummsys.h aliases) live in the global scope so transcribed
//      `byte foo = ...` compiles unchanged.
//   2. Macros / templates: ARRAYSIZE, ABS, MIN, MAX, SWAP, READ_LE_UINT16
//      etc. — same names as scummvm so transcribed code uses them verbatim.
//   3. `namespace Scumm { ... }` is aliased to our `namespace tsb { ... }`,
//      so transcribed files keep their `namespace Scumm {` wrapper but their
//      contents land alongside our existing tsb code.
//   4. `Common::Point` / `Common::Rect` are minimal structs that match
//      scummvm's API surface as used by transcribed files.
//   5. `error()` / `warning()` / `debug()` / `debugC()` route to platform::log.
//   6. ScummEngine accessors that transcribed code touches (`_currentRoom`,
//      `_actors[i]`, `VAR(X)`, `_res->getResourceAddress(rtX, n)`, etc.)
//      are extended file-by-file as we transcribe — start minimal, add the
//      next missing thing the compiler reports.
//
// Anything in this header is HAND-WRITTEN ADAPTER code. Logic itself MUST
// come from scummvm-upstream verbatim.

#pragma once

#include "types.h"
#include "platform.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
// 1. Type aliases (match scummvm-upstream/common/scummsys.h:425-433).
//    Land in global scope so transcribed `byte foo` works without
//    qualification.
// ---------------------------------------------------------------------------

typedef uint8_t  byte;
typedef uint8_t  uint8;
typedef int8_t   int8;
typedef uint16_t uint16;
typedef int16_t  int16;
typedef uint32_t uint32;
typedef int32_t  int32;
typedef uint64_t uint64;
typedef int64_t  int64;
typedef unsigned int uint;

// ---------------------------------------------------------------------------
// 2. Macros / templates (match scummvm-upstream/common/util.h:45-117).
// ---------------------------------------------------------------------------

#ifdef ABS
#  undef ABS
#endif
#ifdef MIN
#  undef MIN
#endif
#ifdef MAX
#  undef MAX
#endif

template<typename T> inline T ABS(T x)        { return (x >= 0) ? x : -x; }
template<typename T> inline T MIN(T a, T b)   { return (a < b) ? a : b; }
template<typename T> inline T MAX(T a, T b)   { return (a > b) ? a : b; }
template<typename T> inline T CLIP(T v, T amin, T amax) {
    return v < amin ? amin : (v > amax ? amax : v);
}
template<typename T> inline void SWAP(T &a, T &b) { T tmp = a; a = b; b = tmp; }

#ifdef ARRAYSIZE
#  undef ARRAYSIZE
#endif
#define ARRAYSIZE(x) ((int)(sizeof(x) / sizeof(x[0])))
#define ARRAYEND(x)  ((x) + ARRAYSIZE((x)))

// LE / BE readers — scummvm-upstream/common/endian.h.
#define READ_LE_UINT16(a) tsb::read_le16((const uint8_t *)(a))
#define READ_LE_UINT32(a) tsb::read_le32((const uint8_t *)(a))
#define READ_BE_UINT16(a) tsb::read_be16((const uint8_t *)(a))
#define READ_BE_UINT32(a) tsb::read_be32((const uint8_t *)(a))
#define WRITE_LE_UINT16(a, v) do {                                  \
        uint16_t _v = (uint16_t)(v); uint8_t *_p = (uint8_t *)(a);  \
        _p[0] = (uint8_t)(_v); _p[1] = (uint8_t)(_v >> 8);          \
    } while (0)
#define WRITE_LE_UINT32(a, v) do {                                  \
        uint32_t _v = (uint32_t)(v); uint8_t *_p = (uint8_t *)(a);  \
        _p[0] = (uint8_t)(_v); _p[1] = (uint8_t)(_v >> 8);          \
        _p[2] = (uint8_t)(_v >> 16); _p[3] = (uint8_t)(_v >> 24);   \
    } while (0)

// scummvm uses these on packed-int fields inside on-disk structs that we
// already store host-endian (we read once on load, not per-access). They
// resolve to identity macros on little-endian builds, which is fine for
// our cortex-m33 + x86-64 host targets.
#define FROM_LE_16(v) ((uint16_t)(v))
#define FROM_LE_32(v) ((uint32_t)(v))
#define TO_LE_16(v)   ((uint16_t)(v))
#define TO_LE_32(v)   ((uint32_t)(v))
#define FROM_BE_16(v) tsb::read_be16(reinterpret_cast<const uint8_t *>(&(v)))
#define FROM_BE_32(v) tsb::read_be32(reinterpret_cast<const uint8_t *>(&(v)))

// ---------------------------------------------------------------------------
// 3. Namespace rewrite — transcribed scummvm code opens `namespace Scumm`.
//    A namespace ALIAS (`namespace Scumm = tsb;`) cannot be used to open a
//    namespace declaration, so we use a preprocessor token rewrite. This
//    makes `namespace Scumm { ... }` textually become `namespace tsb { ... }`
//    in every translation unit that includes this header. Any other
//    occurrence of the bare token `Scumm` (e.g. `using namespace Scumm;`)
//    is rewritten the same way, which is exactly what we want — transcribed
//    code's references to Scumm-namespace symbols all resolve to tsb.
//
//    There is no namespace called `Scumm` after this point; the token does
//    not exist in our compilation.  This is the only "magic" in the shim.
// ---------------------------------------------------------------------------

#define Scumm tsb

// ---------------------------------------------------------------------------
// 4. Minimal Common::Point / Common::Rect (scummvm-upstream/common/rect.h).
//    Only the API surface used by the SCUMM engine itself.
// ---------------------------------------------------------------------------

namespace Common {

struct Point {
    int16_t x;
    int16_t y;

    Point() : x(0), y(0) {}
    Point(int16_t xx, int16_t yy) : x(xx), y(yy) {}

    bool operator==(const Point &o) const { return x == o.x && y == o.y; }
    bool operator!=(const Point &o) const { return !(*this == o); }
};

struct Rect {
    int16_t top;
    int16_t left;
    int16_t bottom;
    int16_t right;

    Rect() : top(0), left(0), bottom(0), right(0) {}
    Rect(int16_t l, int16_t t, int16_t r, int16_t b)
        : top(t), left(l), bottom(b), right(r) {}

    int16_t width()  const { return right - left; }
    int16_t height() const { return bottom - top; }

    bool isEmpty() const { return width() <= 0 || height() <= 0; }
    bool contains(int16_t x, int16_t y) const {
        return x >= left && x < right && y >= top && y < bottom;
    }
    bool contains(const Point &p) const { return contains(p.x, p.y); }
    void clip(const Rect &o) {
        if (top    < o.top)    top    = o.top;
        if (left   < o.left)   left   = o.left;
        if (bottom > o.bottom) bottom = o.bottom;
        if (right  > o.right)  right  = o.right;
    }
    void moveTo(int16_t x, int16_t y) {
        bottom += (y - top);
        right  += (x - left);
        top  = y;
        left = x;
    }
    void translate(int16_t dx, int16_t dy) {
        top    += dy;
        bottom += dy;
        left   += dx;
        right  += dx;
    }
};

}  // namespace Common

// ---------------------------------------------------------------------------
// 5. error() / warning() / debug() — route to platform::log.
//    error() in scummvm aborts; for an embedded port we log + abort so the
//    crash hands us to the bootloader.
// ---------------------------------------------------------------------------

namespace tsb {
[[noreturn]] inline void scummvm_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    platform::log("scummvm error: %s\n", buf);
    abort();
}
}  // namespace tsb

#define error(...)    tsb::scummvm_error(__VA_ARGS__)
#define warning(...)  platform::log(__VA_ARGS__)
#define debug(...)    platform::log(__VA_ARGS__)
#define debugC(...)   ((void)0)         // category-gated debug — silenced

#define assertRange_argDescription_param const char *
// scummvm util.cpp's assertRange uses error() — already covered.

// scummvm uses `assert()` from <cassert>. Keep the standard-library one;
// platform::log will pick up failure via abort handler.
#include <assert.h>
