// Game descriptor table — engine-side implementation.  See
// game_table.h for the data model.  Each game's factory builds a
// DetectorResult from the constants that used to live in main.cpp's
// `#if defined(TSB_GAME_*)` cascade and hands back a constructed
// ScummEngine_v* subclass instance.

#include "scummvm_compat.h"
#include "scumm/scumm.h"
#include "scumm/scumm_v4.h"
#include "scumm/scumm_v5.h"
#include "scumm/detection.h"
#include "common/language.h"
#include "common/platform.h"
#include "common/str.h"

#include "game_table.h"

namespace tsb {

// ---------------- MI1 — Monkey Island 1 VGA Floppy DOS (v4) ---------------
//
// Standard LFL/LEC layout, GF_SMALL_HEADER for v4 disk format.
// GID_MONKEY_VGA triggers the script-152 copy-protection bypass in
// script_v5.cpp.
static ScummEngine *create_mi1(::OSystem *osys) {
    DetectorResult dr;
    dr.game.id        = (int)GID_MONKEY_VGA;
    dr.game.version   = 4;
    dr.game.platform  = Common::kPlatformDOS;
    dr.game.features  = GF_SMALL_HEADER | GF_USE_KEY;
    dr.game.heversion = 0;
    dr.language       = Common::EN_ANY;
    dr.extra          = "";
    dr.md5            = "8e4ee4db46954bfcb6d2654dde0aae25";
    return new ScummEngine_v4(osys, dr);
}

// ---------------- MI2 — Monkey Island 2 LeChuck's Revenge (v5) ------------
//
// Same HD-installed layout family as Indy4: monkey2.000 / monkey2.001.
// MD5 isn't actually validated, but must be a 32-char hex string or
// scumm.cpp's decode loop indexes past the end of an empty string.
static ScummEngine *create_mi2(::OSystem *osys) {
    DetectorResult dr;
    dr.game.id        = (int)GID_MONKEY2;
    dr.game.variant   = "Floppy";
    dr.game.version   = 5;
    dr.game.platform  = Common::kPlatformDOS;
    dr.game.features  = GF_USE_KEY;
    dr.game.heversion = 0;
    dr.language       = Common::EN_ANY;
    dr.extra          = "";
    dr.md5            = "f60039079bcdbfde2dab86bcad9c9c64";
    dr.fp.pattern     = "monkey2.%03d";
    dr.fp.genMethod   = kGenDiskNum;
    return new ScummEngine_v5(osys, dr);
}

// ---------------- INDY3 — Indiana Jones and the Last Crusade EGA (v3) -----
//
// v3 engine class; old LFL layout.  Not yet validated end-to-end —
// engine compiles, but file-resolver in scummvm_link_stubs.cpp lacks
// NN.LFL entries.  Listed here so the picker shows the slot; selecting
// it won't run a game until C3/picker work adds v3 file support.
static ScummEngine *create_indy3(::OSystem *osys) {
    DetectorResult dr;
    dr.game.id        = (int)GID_INDY3;
    dr.game.version   = 3;
    dr.game.platform  = Common::kPlatformDOS;
    dr.game.features  = GF_SMALL_HEADER | GF_USE_KEY;
    dr.game.heversion = 0;
    dr.language       = Common::EN_ANY;
    dr.extra          = "";
    dr.md5            = "";  // unvalidated
    return new ScummEngine_v4(osys, dr);  // v3 inherits from v4 chain
}

// ---------------- INDY4 — Fate of Atlantis Floppy DOS (v5 HD layout) ------
//
// _filenamePattern + kGenDiskNum tells setupScumm to build names
// like "atlantis.000" / "atlantis.001"; the link-stubs file resolver
// maps the .000/.001 suffixes onto data_master_index / data_disk(1).
static ScummEngine *create_indy4(::OSystem *osys) {
    DetectorResult dr;
    dr.game.id        = (int)GID_INDY4;
    dr.game.variant   = "Floppy";   // input.cpp:1098 strcmps this for GID_INDY4
    dr.game.version   = 5;
    dr.game.platform  = Common::kPlatformDOS;
    dr.game.features  = GF_USE_KEY;
    dr.game.heversion = 0;
    dr.language       = Common::EN_ANY;
    dr.extra          = "";
    dr.md5            = "1875b90fade138c9253a8e967007031a";
    dr.fp.pattern     = "atlantis.%03d";
    dr.fp.genMethod   = kGenDiskNum;
    return new ScummEngine_v5(osys, dr);
}

// Per-game file lists.  Null-name sentinel terminates each array.
// xor_byte=0x69 is the LucasArts disk encryption; 0 means plain.  All
// .LFL helpers + the master index are plain on LucasArts floppies;
// only the .LEC archives / .000 / .001 are XOR'd.

static const GameFile kFilesMI1[] = {
    { "000.LFL",    0,    true  },
    { "DISK01.LEC", 0x69, true  },
    { "DISK02.LEC", 0x69, true  },
    { "DISK03.LEC", 0x69, true  },
    { "DISK04.LEC", 0x69, true  },
    { "901.LFL",    0,    false },
    { "902.LFL",    0,    false },
    { "903.LFL",    0,    false },
    { "904.LFL",    0,    false },
    { nullptr,      0,    false },
};

static const GameFile kFilesMI2[] = {
    { "monkey2.000", 0x69, true  },
    { "monkey2.001", 0x69, true  },
    { nullptr,       0,    false },
};

static const GameFile kFilesIndy3[] = {
    // V3_LFL is not yet wired through the engine's file resolver, so
    // listing the per-room NN.LFL files here is premature; the entry
    // is here so future preload + picker work has a clear schema.
    { nullptr, 0, false },
};

static const GameFile kFilesIndy4[] = {
    { "atlantis.000", 0x69, true  },
    { "atlantis.001", 0x69, true  },
    { nullptr,        0,    false },
};

const GameDescriptor kGameTable[] = {
    { "mi1",   "Monkey Island 1",  ContainerVariant::V4_FLOPPY, nullptr,    kFilesMI1,   create_mi1   },
    { "mi2",   "Monkey Island 2",  ContainerVariant::V5_HD,     "monkey2",  kFilesMI2,   create_mi2   },
    { "indy3", "Indiana Jones 3",  ContainerVariant::V3_LFL,    nullptr,    kFilesIndy3, create_indy3 },
    { "indy4", "Indiana Jones 4",  ContainerVariant::V5_HD,     "atlantis", kFilesIndy4, create_indy4 },
};
const int kGameTableCount = sizeof(kGameTable) / sizeof(kGameTable[0]);

// Initial selection.  Standalone builds set TSB_GAME_X via CMake;
// slot builds leave this null and let the platform pick it after
// scanning /scumm/* at boot.
#if defined(TSB_GAME_MI1)
const GameDescriptor *g_current_game = &kGameTable[0];
#elif defined(TSB_GAME_MI2)
const GameDescriptor *g_current_game = &kGameTable[1];
#elif defined(TSB_GAME_INDY3)
const GameDescriptor *g_current_game = &kGameTable[2];
#elif defined(TSB_GAME_INDY4)
const GameDescriptor *g_current_game = &kGameTable[3];
#else
const GameDescriptor *g_current_game = nullptr;
#endif

}  // namespace tsb
