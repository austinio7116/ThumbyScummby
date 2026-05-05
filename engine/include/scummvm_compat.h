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

// Audio::Mixer / SoundHandle come from scummvm-upstream/audio/mixer.h.
#include "audio/mixer.h"

// engines/engine.h provides Engine + PauseToken — we use it as-is.
#include "engines/engine.h"
#include "engines/savestate.h"   // SaveStateDescriptor / SaveStateList
#include "engines/metaengine.h"  // MetaEngine — full def for getMetaEngine() use

// DetectorResult comes from scummvm-upstream/scumm/detection.h via
// scumm/scumm.h's include cone.  We synthesise an instance in main().

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
#include "scumm/he/intern_he.h"   // HE subclasses (real headers, bodies never link)
#include "scumm/bomp.h"           // BompDrawData / drawBomp (referenced by v6+ paths)
#include "scumm/he/resource_he.h" // ResExtractor (cursor.cpp HE path)
#include "scumm/file.h"           // BaseScummFile / ScummFile (charset.cpp opens fonts)
#include "scumm/sound.h"          // Scumm::Sound (real class; stub bodies in scummvm_stubs.cpp)
#include "audio/mixer.h"          // Audio::Mixer / SoundHandle
// scumm/sound.h is NOT included — it pulls audio/mididrv (with MDT_*),
// scumm/soundcd.h methods, AudioCDManager — all things vars.cpp/scumm.cpp
// would also need.  We provide a Sound STUB class below.  Phase 7
// audio_shim.cpp subclasses it with bodies that forward to imuse_*.
#include "scumm/ks_check.h"       // Korean Hangul checkJongsung (string.cpp)
#include "scumm/verbs.h"          // VerbSlot (string.cpp / verbs.cpp)
#include "graphics/font.h"        // Graphics::Font (charset.cpp Mac font path)
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

// audio/musicplugin.h MusicEngine — abstract base.  Stubbed for parse only.
class MusicEngine {
public:
    virtual ~MusicEngine() {}
    virtual void setMusicVolume(int) {}
    virtual void setSfxVolume(int) {}
    virtual void startSound(int) {}
    virtual void stopSound(int) {}
    virtual void stopAllSounds() {}
    virtual int  getSoundStatus(int) const { return 0; }
    virtual int  getMusicTimer() { return 0; }
    virtual void terminate() {}
    virtual int  getQueueSize() { return 0; }
    virtual void setQuality(int) {}
    virtual void toggleMusic(bool) {}
    virtual void toggleSoundEffects(bool) {}
    virtual void restoreAfterLoad() {}
};

// scummvm-upstream/scumm/imuse/imuse.h.  Replaced by our imuse.cpp;
// `_imuse` member stays nullptr.  Inherits MusicEngine because v6+
// scumm.cpp assigns `_musicEngine = imuse`.
class IMuse : public MusicEngine {
public:
    virtual ~IMuse() {}
    static IMuse *create(ScummEngine *, MidiDriver *, MidiDriver *, MidiDriverFlags, bool) { return nullptr; }
    void addSysexHandler(byte, void (*)(MidiDriver_BASE *, const byte *, uint16)) {}

    enum {
        PROP_TEMPO_BASE = 1,
        PROP_NATIVE_MT32 = 2,
        PROP_GS = 3,
        PROP_LIMIT_PLAYERS = 4,
        PROP_RECYCLE_PLAYERS = 5,
        PROP_GAME_ID = 6,
        PROP_RHYTHM_CHANNEL = 7,
        PROP_DIRECT_PASSTHROUGH = 8,
    };
    uint32 property(int, uint32) { return 0; }
};

// Sysex handlers for SCUMM games (used as function pointers).
inline void sysexHandler_Scumm(MidiDriver_BASE *, const byte *, uint16) {}
inline void sysexHandler_SamNMax(MidiDriver_BASE *, const byte *, uint16) {}

// FT/DIG/COMI digital iMUSE — disabled.
class IMuseDigital { public: virtual ~IMuseDigital() {} };

// FM-Towns specific — disabled.  Inherits MusicEngine so v6+ assignment works.
class Player_Towns : public MusicEngine {
public:
    virtual ~Player_Towns() {}
    virtual bool init() { return false; }
    virtual void setVolumeCD(int, int) {}
    virtual void setSoundVolume(int, int, int) {}
    virtual void setSoundNote(int, int) {}
    virtual int  getCurrentCdaVolume() { return 0; }
};

// Mac GUI — disabled.  Methods called from transcribed code are stubbed.
class MacGui {
public:
    virtual ~MacGui() {}
    virtual void setPaletteDirty() {}
    virtual const Graphics::Font *getFontByScummId(int) { return nullptr; }
    virtual void printCharToTextArea(int, int, int, int) {}
    virtual void setupCursor(int /*&width*/, int /*&height*/, int /*&hotspotX*/, int /*&hotspotY*/, int /*&animate*/) {}
    virtual void initTextAreaForActor(Actor *, int) {}
    virtual void update(int /*delta*/) {}
    virtual void updateWindowManager() {}
    virtual bool isVerbGuiActive() const { return false; }
    virtual bool handleEvent(const Common::Event &) { return false; }
    virtual bool runQuitDialog() { return true; }
    virtual bool runRestartDialog() { return true; }
    virtual void runDraftsInventory() {}

    // ctors used by transcribed scumm.cpp
    MacGui() = default;
    MacGui(ScummEngine *) {}
    MacGui(ScummEngine *, const Common::Path &) {}

    virtual bool initialize() { return true; }
    virtual void clearTextArea() {}
    virtual void reset() {}
};

// SoundHE — minimal subclass with playVoice for HE talkie path.
// SoundHE class is defined further down (after Sound) but methods declared
// here so MacGui sequence parses.

}  // close Scumm namespace momentarily for global GUI::Dialog stub

// scumm/dialogs.h InfoDialog inherits from GUI::Dialog (in
// scummvm-upstream/gui/dialog.h).  We don't ship the GUI subsystem.
namespace GUI {
class Dialog {
public:
    virtual ~Dialog() {}
    virtual int runModal() { return 0; }
    Common::U32String _backgroundType;     // unused
};
class MessageDialog : public Dialog {
public:
    MessageDialog(const Common::U32String &) {}
    MessageDialog(const Common::String &) {}
    MessageDialog(const Common::U32String &, const Common::U32String &) {}
    MessageDialog(const Common::U32String &, const Common::U32String &, const Common::U32String &) {}
    MessageDialog(const Common::String &, const Common::String &, const Common::String &) {}
    MessageDialog(const char *, const Common::U32String &, const Common::U32String &) {}
    enum Result { kOK = 0, kCancel = 1 };
};
}

namespace Scumm {

class ScummDialog : public GUI::Dialog {};

class InfoDialog : public ScummDialog {
public:
    InfoDialog(ScummEngine *, const Common::U32String &) {}
    InfoDialog(ScummEngine *, const Common::String &) {}
    InfoDialog(ScummEngine *, int) {}
    void setInfoText(const Common::U32String &) {}
};

class SubtitleSettingsDialog : public InfoDialog {
public:
    SubtitleSettingsDialog(ScummEngine *vm, int) : InfoDialog(vm, 0) {}
    int getValue() const { return 0; }
};

class Indy3IQPointsDialog : public InfoDialog {
public:
    Indy3IQPointsDialog(ScummEngine *vm, char *) : InfoDialog(vm, 0) {}
};

class PauseDialog : public InfoDialog {
public:
    PauseDialog(ScummEngine *vm, int) : InfoDialog(vm, 0) {}
};

class ConfirmDialog : public InfoDialog {
public:
    ConfirmDialog(ScummEngine *vm, int) : InfoDialog(vm, 0) {}
};

class ValueDisplayDialog : public GUI::Dialog {
public:
    ValueDisplayDialog(const Common::U32String &, int, int, int, char, char) {}
    int getValue() const { return 0; }
    void setValue(int) {}
};

// Sound — use scummvm-upstream's real class via scumm/sound.h (included
// at top of file).  Method bodies are no-op stubs in scummvm_stubs.cpp.
// Phase 7 audio_shim.cpp subclasses with bodies that forward to imuse_*.

// SoundHE — HE Sound subclass.  Stub — never instantiated.
class SoundHE : public Sound {
public:
    SoundHE(ScummEngine *vm) : Sound(vm, nullptr, false) {}
    SoundHE(ScummEngine *vm, Audio::Mixer *m, Common::Mutex *)
        : Sound(vm, m, false) {}
    void playVoice(uint32, uint32) {}
    void feedMixer() {}
    void handleSoundFrame() {}
};

// scumm/sound_he.h: HSND_TALKIE_SLOT.
enum {
    HSND_TALKIE_SLOT = 1,
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
