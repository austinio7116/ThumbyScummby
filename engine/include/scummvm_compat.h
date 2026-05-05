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

    // Squared distance — matches scummvm-upstream/common/rect.h.
    unsigned int sqrDist(const Point &p) const {
        int dx = x - p.x;
        int dy = y - p.y;
        return (unsigned int)(dx * dx + dy * dy);
    }
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

// scummvm's `debug(level, fmt, ...)` and `debugC(level, channel, fmt, ...)`:
// drop the leading level/channel args before forwarding to platform::log.
namespace tsb {
inline void scummvm_debug_impl(int /*level*/, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    platform::log("%s\n", buf);
}
}
#define debug(...)    tsb::scummvm_debug_impl(__VA_ARGS__)
#define debugC(...)   ((void)0)         // category-gated debug — silenced

#define assertRange_argDescription_param const char *
// scummvm util.cpp's assertRange uses error() — already covered.

// scummvm uses `assert()` from <cassert>. Keep the standard-library one;
// platform::log will pick up failure via abort handler.
#include <assert.h>

// ---------------------------------------------------------------------------
// 6. Engine state declarations (ScummEngine + Actor scaffolding).
//
// These are HAND-WRITTEN class declarations that match the API surface
// transcribed scummvm sources expect. Each class lists ONLY the data
// members and method signatures used by transcribed code — NOT a full
// scummvm class hierarchy. Members are appended as files are transcribed.
//
// Method bodies live in the transcribed .cpp files (e.g. ScummEngine::
// getMaskFromBox is defined in src/boxes.cpp). Only members whose bodies
// would duplicate transcribed logic — e.g. `enhancementEnabled` returning
// a constant — get inlined here.
// ---------------------------------------------------------------------------

namespace tsb {

// Game / platform / feature constants (subset — matches
// scummvm-upstream/engines/scumm/scumm.h).
enum GameId {
    GID_MANIAC = 1,
    GID_ZAK = 2,
    GID_INDY3 = 3,
    GID_LOOM = 4,
    GID_PASS = 5,
    GID_MONKEY_EGA = 6,
    GID_MONKEY_VGA = 7,
    GID_MONKEY = 8,
    GID_MONKEY2 = 9,
    GID_INDY4 = 10,
    GID_TENTACLE = 11,
    GID_SAMNMAX = 12,
    GID_FT = 13,
    GID_DIG = 14,
    GID_CMI = 15,
};

enum GameFeatures {
    GF_SMALL_HEADER          = 1 << 0,   // v3-v4
    GF_OLD_BUNDLE            = 1 << 1,
    GF_NEW_COSTUMES          = 1 << 6,
    GF_FEW_LOCALS            = 1 << 8,
    GF_AMIGA                 = 1 << 14,
    GF_USE_KEY               = 1 << 16,
};

enum ResourceType {
    rtRoom = 1,
    rtScript,
    rtCostume,
    rtSound,
    rtInventory,
    rtCharset,
    rtString,
    rtVerb,
    rtActorName,
    rtBuffer,
    rtScaleTable,
    rtTemp,
    rtFlObject,
    rtMatrix,
    rtBox,
    rtObjectName,
    rtRoomScripts,
    rtRoomImage,
    rtImage,
    rtTalkie,
    rtSpoolBuffer,
    rtNumTypes,
};

enum {
    kEnhMinorBugFixes        = 1,
    kEnhGameBreakingBugFixes = 2,
    kEnhGameRestoredFixes    = 3,
    kEnhVisualChanges        = 4,
    kEnhAudioChanges         = 5,
    kEnhSubFmtCntChanges     = 6,
    kEnhTextLocFixes         = 7,
    kEnhGrp1                 = 8,
    kEnhGrp2                 = 9,
    kEnhGrp3                 = 10,
    kEnhGrp4                 = 11,
};

// Platform constants moved into namespace Common at global scope (see
// section 4 above) — extending here so transcribed code's
// Common::kPlatformDOS resolves alongside Common::Point/Common::Rect.
}  // close `namespace tsb` momentarily so we can extend ::Common
namespace Common {
    enum Platform {
        kPlatformDOS         = 1,
        kPlatformAmiga       = 2,
        kPlatformAtariST     = 3,
        kPlatformMacintosh   = 4,
        kPlatformFMTowns     = 5,
        kPlatformWindows     = 6,
        kPlatformNES         = 7,
        kPlatformC64         = 8,
        kPlatformSegaCD      = 9,
        kPlatformPCEngine    = 10,
        kPlatform3DO         = 11,
    };
}
namespace tsb {

struct GameSettings {
    uint8_t  version;       // 4 for MI1 VGA Floppy, 5 for MI2 etc.
    uint8_t  id;            // GID_*
    uint16_t platform;      // Common::kPlatform*
    uint32_t features;      // GF_* bitmask
    uint8_t  heversion;     // 0 for non-HE
};

// ---- ScaleSlot mirrors scummvm-upstream/scumm.h ScaleSlot. ----
struct ScaleSlot {
    int x1, y1, scale1;
    int x2, y2, scale2;
};

// kOldInvalidBox / kInvalidBox — scummvm-upstream/actor.h.
// kInvalidBox is for v3+ (255), kOldInvalidBox is for v0-v2 (0).
constexpr int kOldInvalidBox = 0;
// Defined in struct Actor below as static constexpr; also expose
// at namespace scope for code that uses Actor::kInvalidBox.

// Forward declarations.
struct Actor;
struct Box;             // packed struct defined in boxes.cpp
struct BoxCoords {      // matches scummvm-upstream/boxes.h
    Common::Point ul;
    Common::Point ur;
    Common::Point ll;
    Common::Point lr;
};

// scummvm-upstream/scumm/boxes.h sizes (per-version).
#define SIZEOF_BOX_V0 5
#define SIZEOF_BOX_V2 8
#define SIZEOF_BOX_V3 18
#define SIZEOF_BOX    20
#define SIZEOF_BOX_V8 52

// scummvm-upstream/scumm/boxes.h BoxFlags.
enum BoxFlags {
    kBoxXFlip       = 0x08,
    kBoxYFlip       = 0x10,
    kBoxIgnoreScale = 0x20,
    kBoxPlayerOnly  = 0x20,
    kBoxLocked      = 0x40,
    kBoxInvisible   = 0x80,
};

int getClosestPtOnBox(const BoxCoords &box, int x, int y, int16_t &outX, int16_t &outY);

// scummvm-upstream/scumm/util.h.
void assertRange(int min, int value, int max, const char *desc);
int  newDirToOldDir(int dir);
int  oldDirToNewDir(int dir);
int  toSimpleDir(int dirType, int dir);
int  fromSimpleDir(int dirType, int dir);
int  normalizeAngle(int dirType, int angle);

// scummvm Resource manager stub. createResource(rtMatrix, 1, size) is the
// only path our v4 transcribed code currently exercises (createBoxMatrix
// allocates BOXM there).  We back rtMatrix slot 1 with the same buffer
// getResourceAddress(rtMatrix, 1) returns.
class Resources {
public:
    uint8_t *createResource(int type, int idx, size_t size);
};
extern Resources g_resources;

class ScummEngine {
public:
    ScummEngine();

    // ---- Game state (set at engine init / on room change) ----
    GameSettings _game;
    int _currentRoom;
    int _roomResource;

    // ---- Scale slots: mirrors scummvm _scaleSlots[20] ----
    ScaleSlot _scaleSlots[20];

    // ---- Resource pool: scummvm has a real resource manager. We back
    //      rtMatrix slot 1 (BOXM) and slot 2 (BOXD) with writable
    //      buffers loaded from the room on each room change. ----
    static constexpr size_t BOX_DATA_BUF_SIZE   = 1280;   // 64 * SIZEOF_BOX
    static constexpr size_t BOX_MATRIX_BUF_SIZE = 2000;
    uint8_t _boxDataBuf[BOX_DATA_BUF_SIZE];
    int     _boxDataSize;       // bytes actually used
    uint8_t _boxMatrixBuf[BOX_MATRIX_BUF_SIZE];
    int     _boxMatrixSize;

    // _extraBoxFlags is v7+ only — empty for our purposes.
    int _extraBoxFlags[65];

    // ---- ScummEngine API used by boxes.cpp / actor.cpp transcription.
    //      Bodies live in the transcribed sources; declarations here. ----
    uint8_t getMaskFromBox(int box);
    void    setBoxFlags(int box, int val);
    uint8_t getBoxFlags(int box);
    void    setBoxScale(int box, int scale);
    void    setBoxScaleSlot(int box, int slot);
    int     getScale(int box, int x, int y);
    int     getScaleFromSlot(int slot, int x, int y);
    int     getBoxScale(int box);
    void    convertScaleTableToScaleSlot(int slot);
    void    setScaleSlot(int slot, int x1, int y1, int scale1, int x2, int y2, int scale2);
    uint8_t getNumBoxes();
    Box    *getBoxBaseAddr(int box);
    bool    checkXYInBoxBounds(int boxnum, int x, int y);
    BoxCoords getBoxCoordinates(int boxnum);
    uint8_t *getBoxMatrixBaseAddr();
    uint8_t *getBoxConnectionBase(int box);
    int     getNextBox(uint8_t from, uint8_t to);
    void    calcItineraryMatrix(uint8_t *itineraryMatrix, int num);
    void    createBoxMatrix();
    bool    areBoxesNeighbors(int box1nr, int box2nr);

    // Stubbed: scummvm has `_res->getResourceAddress(type, idx)`. We
    // forward only the slots transcribed code asks for; everything else
    // returns nullptr for now. Implementation in scummvm_compat.cpp.
    uint8_t *getResourceAddress(int type, int idx);
    int      getResourceSize(int type, int idx);

    // Enhancement gates: scummvm has a config-driven set of bug-fix
    // toggles. For an embedded port we always enable canonical (= no
    // workaround). Transcribed code stays untouched.
    bool enhancementEnabled(int /*group*/) { return false; }

    // VAR access — scummvm has VAR(x) macro that resolves to
    // _scummVars[x]. Plumb through to our existing g_vm.globals.
    int32_t *_scummVars;        // points at g_vm.globals[]

    // scummvm: ScummEngine has `ResourceManager *_res;`.  We provide a
    // tiny Resources facade — see Resources class above.
    Resources *_res;
};

extern ScummEngine *g_scumm;

// ---- Actor scaffolding (will grow as actor.cpp is transcribed). ----
//
// Defined as a *class* (matching scummvm) with public members so transcribed
// code's `_pos.x = ...` / `a->_walkbox = ...` works unchanged.
class Actor {
public:
    static constexpr int kInvalidBox = 0xFF;
    // Members will be added file-by-file when actor.cpp / boxes.cpp need
    // them.  At present transcribed boxes.cpp's Actor::findPathTowards is
    // moved to actor.cpp transcription — so this class needs nothing yet.
};

}  // namespace tsb

// Inline a scummvm `VAR(x)` macro that resolves to g_scumm->_scummVars[x].
#define VAR(x) (tsb::g_scumm->_scummVars[(x)])
