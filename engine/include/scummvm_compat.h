// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — scummvm-upstream compatibility shim (post-pivot).
//
// After OSYSTEM_PIVOT_PLAN.md, this header is no longer the authoritative
// declaration of ScummEngine.  scumm/scumm.h is.  This file:
//
//   1. Pulls in scummvm common/graphics/engines/scumm headers — so
//      transcribed code's `Common::Point`, `Graphics::Surface`,
//      `ScummEngine`, `Sound`, `Gdi`, etc. resolve to the real ones.
//   2. Provides stubs for upstream classes we *don't* compile in (MacGui,
//      IMuseDigital, MusicEngine, Player_Towns) and the OSystem / Engine
//      base classes that we replace with OSystem_Thumby.
//   3. Routes scummvm's `error()` / `warning()` / `debug()` through our
//      platform::log so we keep one logging path.
//   4. Aliases `namespace Scumm` -> `namespace tsb` via preprocessor
//      rewrite so transcribed `namespace Scumm { ... }` lands in tsb.
//
// Anything that *adds new logic* belongs in osystem_thumby.cpp or
// audio_shim.cpp — NOT here.

#pragma once

#include "types.h"
#include "platform.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <new>

// ---------------------------------------------------------------------------
// 1. Pull in scummvm common.  Provides byte/uint16/ARRAYSIZE/MIN/MAX/ABS/
//    READ_LE_UINT16/CLIP/SWAP, plus Common::Point/Rect/String/Path/Array/
//    HashMap/Random/Serializer/etc.
// ---------------------------------------------------------------------------

#include "common/scummsys.h"        // byte, uint16, etc.
#include "common/endian.h"          // READ_LE_UINT16, FROM_LE_*, etc.
#include "common/util.h"            // ABS / MIN / MAX / CLIP / SWAP
#include "common/rect.h"            // Common::Point, Common::Rect
#include "common/array.h"
#include "common/list.h"
#include "common/hashmap.h"
#include "common/queue.h"
#include "common/stack.h"
#include "common/random.h"
#include "common/serializer.h"
#include "common/str.h"
#include "common/path.h"
#include "common/keyboard.h"
#include "common/events.h"
#include "common/textconsole.h"     // debug / warning / error
#include "common/error.h"
#include "common/stream.h"
#include "common/memstream.h"

// scummvm's `error()` aborts; for embedded we want to log + halt. The
// scummvm textconsole.cpp impl already does `error_handler(msg); abort();`
// — we just need to install a handler that goes through platform::log.
// Done in osystem_thumby.cpp.

// ---------------------------------------------------------------------------
// 2. Pull in scummvm graphics.  Provides Graphics::Surface, PixelFormat,
//    PaletteLookup.  Used by Gdi / virtscreens.
// ---------------------------------------------------------------------------

#include "graphics/surface.h"
#include "graphics/pixelformat.h"
#include "graphics/palette.h"
#include "graphics/paletteman.h"

// Stub class definitions live AFTER `#define Scumm tsb` (section 5+6
// below), so they land in the `tsb` namespace and match scumm.h's
// rewritten declarations.  See section 6.5.

// ---------------------------------------------------------------------------
// 4. Stub the engines/Engine base class.  scumm/scumm.h declares
//    `class ScummEngine : public Engine, public Common::Serializable`.
//    Engine (engines/engine.h) is a heavy abstract base with PauseToken,
//    AchievementsManager, MetaEngine, etc. — way too much to import.
//    We provide a minimal no-op stub at the same name with the methods
//    ScummEngine overrides.
// ---------------------------------------------------------------------------

class OSystem;          // forward — full def in osystem_thumby.h.
class MetaEngine;
class MetaEngineDetection;

namespace Common {
class Keymap;
class Keymapper;
class TextToSpeechManager;
class SaveFileManager;
class EventManager;
class TimerManager;
}

namespace Audio { class Mixer; }

// engines/engine.h provides Engine + PauseToken — we use it as-is.
#include "engines/engine.h"

// engines/metaengine.h: DetectorResult.  scumm.h's ScummEngine ctor takes
// `(OSystem *, const DetectorResult &)`.  Real scummvm fills DetectorResult
// from the detection pass; we synthesise one in OSystem_Thumby init.
namespace Scumm {
struct DetectorResult {
    int gameId = 0;
    int features = 0;
    int platform = 0;
    int version = 4;
    int variant = 0;
    int language = 0;
    Common::Path fsPath;
    Common::String md5;
    Common::String extra;
    int  midi = 0;
    bool guiOptions = false;
};
}

// ---------------------------------------------------------------------------
// 5. Namespace token rewrite — transcribed scummvm code opens
//    `namespace Scumm { ... }`. We want it to land in our `tsb` namespace.
//    Keep this AT THE END so scumm.h's class declarations use the rewritten
//    name.
// ---------------------------------------------------------------------------

#define Scumm tsb

// ---------------------------------------------------------------------------
// 6. Pull in the real ScummEngine.  After this point, Scumm::ScummEngine
//    (== tsb::ScummEngine after the rewrite) is the canonical class with
//    all 1100+ members from scumm.h.
//
// scumm.h includes engines/engine.h which forward-declares OSystem, etc.
// Those are stubbed above.
// ---------------------------------------------------------------------------

#include "scumm/scumm.h"
#include "scumm/boxes.h"
#include "scumm/util.h"
#include "scumm/object.h"
#include "scumm/usage_bits.h"
#include "scumm/resource.h"
#include "scumm/actor.h"
#include "scumm/charset.h"

// Forward declarations for ScummEngine_v0..v8 — transcribed cpp files
// have method bodies for all version subclasses, but we only ship
// ScummEngine_v4 / ScummEngine_v5 (transcribed at scumm/scumm_v4.h /
// scumm_v5.h).  Stub the others as empty subclasses so member-method
// definitions like `void ScummEngine_v6::palManipulate()` parse.
#include "scumm/scumm_v0.h"   // stub
#include "scumm/scumm_v4.h"   // real
#include "scumm/scumm_v5.h"   // real
#include "scumm/scumm_v6.h"   // stub
#include "scumm/scumm_v7.h"   // stub
#include "scumm/scumm_v8.h"   // stub
// scumm/sound.h pulls in audio/mididrv.h + scumm/{soundcd,soundse}.h —
// we replace it with our own minimal Sound class via audio_shim.cpp.
// Forward decl below is sufficient for ScummEngine's _sound member.

// scumm/he/actor_he.h — defines HE100_CHORE_REDIRECT_*.  The HE100 branch
// is dead code for our v4 games (heversion < 99), but actor.cpp's
// startAnimActor references the macros so we forward them here.
#define HE100_CHORE_REDIRECT_INIT        1024
#define HE100_CHORE_REDIRECT_WALK        1025
#define HE100_CHORE_REDIRECT_STAND       1026
#define HE100_CHORE_REDIRECT_START_TALK  1027
#define HE100_CHORE_REDIRECT_STOP_TALK   1028

// ---------------------------------------------------------------------------
// 6.5. Stub class definitions for upstream subsystems we *don't* compile.
//      Land in `namespace Scumm` (which is now tsb) so they match scumm.h's
//      forward declarations and ScummEngine member pointers resolve.
//
//      Sound has methods called from transcribed code (actor.cpp,
//      script_v5.cpp, etc.) — bodies stubbed here, real impl swapped in
//      via audio_shim.cpp later.
// ---------------------------------------------------------------------------

namespace Scumm {        // == namespace tsb after rewrite

// scummvm-upstream/scumm/imuse/imuse.h.  Replaced by our imuse.cpp;
// `_imuse` member stays nullptr.
class IMuse { public: virtual ~IMuse() {} };

// FT/DIG/COMI digital iMUSE — disabled.
class IMuseDigital { public: virtual ~IMuseDigital() {} };

// audio/musicplugin.h MusicEngine — disabled.
class MusicEngine { public: virtual ~MusicEngine() {} };

// FM-Towns specific — disabled.
class Player_Towns { public: virtual ~Player_Towns() {} };

// Mac GUI — disabled.  Methods called from transcribed code are stubbed.
class MacGui {
public:
    virtual ~MacGui() {}
    virtual void setPaletteDirty() {}
    virtual void *getFontByScummId(int) { return nullptr; }
    virtual void printCharToTextArea(int, int, int, int) {}
    virtual void setupCursor(int /*&width*/, int /*&height*/, int /*&hotspotX*/, int /*&hotspotY*/, int /*&animate*/) {}
};

// Sound — bodies in audio_shim.cpp forward to imuse_*.  Methods listed
// here are the ones transcribed code currently calls.
class Sound {
public:
    Sound(ScummEngine *vm) : _vm(vm) {}
    virtual ~Sound() {}

    virtual void startSound(int sound, int heOffset = 0,
                            int heChannel = 0, int heFlags = 0,
                            int heFreq = 0, int hePan = 0, int heVol = 0) {}
    virtual void stopSound(int sound) {}
    virtual void stopAllSounds() {}
    virtual bool isSoundRunning(int sound) const { return false; }
    virtual bool isSoundInQueue(int sound) const { return false; }
    virtual int  getSoundElapsedTime(int sound) const { return 0; }
    virtual int  isSoundRunningEgo(int sound, int actor) const { return 0; }
    virtual void soundKludge(int *list, int num) {}
    virtual void talkSound(uint32 a, uint32 b, int mode, int channel = 0) {}
    virtual void processSound() {}
    virtual void pauseSounds(bool pause) {}
    virtual void setupSound() {}
    virtual void modifySound(int sound, int offset, int data, int type) {}
    virtual void addSoundToQueue(int sound, int heOffset = 0,
                                 int heChannel = 0, int heFlags = 0,
                                 int heFreq = 0, int hePan = 0, int heVol = 0) {}
    virtual void addSoundToQueue2(int sound, int heOffset = 0,
                                  int heChannel = 0, int heFlags = 0) {}

protected:
    ScummEngine *_vm;
};

}  // namespace Scumm (tsb after rewrite)

// ---------------------------------------------------------------------------
// 6.6. Other stubs for global / Common:: / Graphics:: symbols transcribed
//      code references but we don't fully provide.
// ---------------------------------------------------------------------------

// scummvm's common/config-manager.h is the real ConfigManager.  We
// don't compile config-manager.cpp (it pulls fs.h / file.h / system.h),
// so transcribed code that uses ConfMan.getBool(...) etc. will hit
// link errors.  scummvm_stubs.cpp provides empty bodies.
#include "common/config-manager.h"

// scummvm-upstream/graphics/macega.h — Mac gamma table.  We don't render
// for Mac, so a 256-byte identity table is fine.
namespace Graphics {
extern const byte macGammaCorrectionLookUp[256];
}

// ---------------------------------------------------------------------------
// 7. ThumbyScummby-side glue.  These exist solely to bridge our existing
//    chunk readers / VM into the new engine plumbing.  When OSystem_Thumby
//    + main.cpp rewrite are done, most of this disappears.
// ---------------------------------------------------------------------------

namespace Scumm {        // == namespace tsb after rewrite

// Our singleton ScummEngine.  Real scummvm doesn't use a singleton — the
// engine is created by main.cpp.  Until OSystem_Thumby lands we keep this
// pointer for legacy bridge code (scummvm_compat.cpp).
extern ScummEngine *g_scumm;

// Minimal Resources facade — resourceManager replacement during pivot.
// Goes away when scummvm's ResourceManager class is wired in scumm.cpp.
class Resources {
public:
    uint8_t *createResource(int type, int idx, size_t size);
};
extern Resources g_resources;

// Bridge entry points used by our existing engine.cpp during pivot.
// These shrink/disappear as more scummvm code comes online.
void scummvm_compat_init();
void scummvm_compat_room_change(int new_room, int room_resource,
                                Span boxd_payload, Span boxm_payload,
                                Span scal_payload);

}  // namespace Scumm  (tsb after rewrite)
