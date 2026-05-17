// Game descriptor table — engine-side implementation.  See
// game_table.h for the data model.  Each game's factory builds a
// DetectorResult from the constants that used to live in main.cpp's
// `#if defined(TSB_GAME_*)` cascade and hands back a constructed
// ScummEngine_v* subclass instance.

#include "scummvm_compat.h"
#include "scumm/scumm.h"
#include "scumm/scumm_v3.h"
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
    // Value-initialise + fill every pointer.  ScummEngine::setupScumm
    // strcmps _game.gameid unconditionally (scumm.cpp:268) and
    // Common::String(_game.guioptions).contains() expects a valid
    // C string; nullptr here hardfaults during engine init.
    DetectorResult dr = {};
    dr.game.gameid       = "monkey";
    dr.game.variant      = "";
    dr.game.preferredTag = "";
    dr.game.guioptions   = "";
    dr.game.id           = (int)GID_MONKEY_VGA;
    dr.game.version      = 4;
    dr.game.platform     = Common::kPlatformDOS;
    dr.game.features     = GF_SMALL_HEADER | GF_USE_KEY;
    dr.game.heversion    = 0;
    dr.language          = Common::EN_ANY;
    dr.extra             = "";
    dr.md5               = "8e4ee4db46954bfcb6d2654dde0aae25";
    return new ScummEngine_v4(osys, dr);
}

// ---------------- MI2 — Monkey Island 2 LeChuck's Revenge (v5) ------------
//
// Same HD-installed layout family as Indy4: monkey2.000 / monkey2.001.
// MD5 isn't actually validated, but must be a 32-char hex string or
// scumm.cpp's decode loop indexes past the end of an empty string.
static ScummEngine *create_mi2(::OSystem *osys) {
    DetectorResult dr = {};
    dr.game.gameid       = "monkey2";
    dr.game.variant      = "Floppy";
    dr.game.preferredTag = "";
    dr.game.guioptions   = "";
    dr.game.id           = (int)GID_MONKEY2;
    dr.game.version      = 5;
    dr.game.platform     = Common::kPlatformDOS;
    dr.game.features     = GF_USE_KEY;
    dr.game.heversion    = 0;
    dr.language          = Common::EN_ANY;
    dr.extra             = "";
    dr.md5               = "f60039079bcdbfde2dab86bcad9c9c64";
    dr.fp.pattern        = "monkey2.%03d";
    dr.fp.genMethod      = kGenDiskNum;
    return new ScummEngine_v5(osys, dr);
}

// ---------------- INDY3 — Indiana Jones and the Last Crusade EGA (v3) -----
//
// v3 engine class for the EGA floppy release.  Uses the per-room LFL
// layout (00.LFL master + NN.LFL room files) wired through
// resolve_game_file in scummvm_link_stubs.cpp + the V3_LFL branch of
// platform_pico's try_descriptor.  GF_USE_KEY triggers ScummVM's
// 0xFF XOR on resource reads — files in /scumm/indy3/ must be
// pre-decrypted by the preload pass (xor_byte 0xFF in kFilesIndy3
// below) since the device build forces setEnc to no-op (see
// scumm/file.h's THUMBY-PORT override).
static ScummEngine *create_indy3(::OSystem *osys) {
    DetectorResult dr = {};
    dr.game.gameid       = "indy3";
    dr.game.variant      = "";
    dr.game.preferredTag = "";
    dr.game.guioptions   = "";
    dr.game.id           = (int)GID_INDY3;
    dr.game.version      = 3;
    dr.game.platform     = Common::kPlatformDOS;
    dr.game.features     = GF_SMALL_HEADER | GF_USE_KEY;
    dr.game.heversion    = 0;
    dr.language          = Common::EN_ANY;
    dr.extra             = "";
    dr.md5               = "";  // unvalidated
    return new ScummEngine_v3(osys, dr);
}

// ---------------- INDY4 — Fate of Atlantis Floppy DOS (v5 HD layout) ------
//
// _filenamePattern + kGenDiskNum tells setupScumm to build names
// like "atlantis.000" / "atlantis.001"; the link-stubs file resolver
// maps the .000/.001 suffixes onto data_master_index / data_disk(1).
static ScummEngine *create_indy4(::OSystem *osys) {
    DetectorResult dr = {};
    dr.game.gameid       = "atlantis";
    dr.game.variant      = "Floppy";  // input.cpp:1098 strcmps this for GID_INDY4
    dr.game.preferredTag = "";
    dr.game.guioptions   = "";
    dr.game.id           = (int)GID_INDY4;
    dr.game.version      = 5;
    dr.game.platform     = Common::kPlatformDOS;
    dr.game.features     = GF_USE_KEY;
    dr.game.heversion    = 0;
    dr.language          = Common::EN_ANY;
    dr.extra             = "";
    dr.md5               = "1875b90fade138c9253a8e967007031a";
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

// V3 LFL files for Indy 3 EGA.  Encryption byte is 0xFF (ScummVM's
// canonical v3 XOR key) — preload decrypts each present file in
// place on first boot.  Only 00.LFL (the master index) is marked
// required so the descriptor reports as installed even when the
// original floppies ship sparse room sets (rooms 5, 10, 11, etc.
// are intentional gaps in Indy 3 EGA).  Rooms 1..62 cover all the
// per-room files ScummVM might call openResourceFile on at runtime;
// missing ones drop through the resolver's empty-Span path.
static const GameFile kFilesIndy3[] = {
    { "00.LFL", 0xFF, true  },
    { "01.LFL", 0xFF, false }, { "02.LFL", 0xFF, false },
    { "03.LFL", 0xFF, false }, { "04.LFL", 0xFF, false },
    { "05.LFL", 0xFF, false }, { "06.LFL", 0xFF, false },
    { "07.LFL", 0xFF, false }, { "08.LFL", 0xFF, false },
    { "09.LFL", 0xFF, false }, { "10.LFL", 0xFF, false },
    { "11.LFL", 0xFF, false }, { "12.LFL", 0xFF, false },
    { "13.LFL", 0xFF, false }, { "14.LFL", 0xFF, false },
    { "15.LFL", 0xFF, false }, { "16.LFL", 0xFF, false },
    { "17.LFL", 0xFF, false }, { "18.LFL", 0xFF, false },
    { "19.LFL", 0xFF, false }, { "20.LFL", 0xFF, false },
    { "21.LFL", 0xFF, false }, { "22.LFL", 0xFF, false },
    { "23.LFL", 0xFF, false }, { "24.LFL", 0xFF, false },
    { "25.LFL", 0xFF, false }, { "26.LFL", 0xFF, false },
    { "27.LFL", 0xFF, false }, { "28.LFL", 0xFF, false },
    { "29.LFL", 0xFF, false }, { "30.LFL", 0xFF, false },
    { "31.LFL", 0xFF, false }, { "32.LFL", 0xFF, false },
    { "33.LFL", 0xFF, false }, { "34.LFL", 0xFF, false },
    { "35.LFL", 0xFF, false }, { "36.LFL", 0xFF, false },
    { "37.LFL", 0xFF, false }, { "38.LFL", 0xFF, false },
    { "39.LFL", 0xFF, false }, { "40.LFL", 0xFF, false },
    { "41.LFL", 0xFF, false }, { "42.LFL", 0xFF, false },
    { "43.LFL", 0xFF, false }, { "44.LFL", 0xFF, false },
    { "45.LFL", 0xFF, false }, { "46.LFL", 0xFF, false },
    { "47.LFL", 0xFF, false }, { "48.LFL", 0xFF, false },
    { "49.LFL", 0xFF, false }, { "50.LFL", 0xFF, false },
    { "51.LFL", 0xFF, false }, { "52.LFL", 0xFF, false },
    { "53.LFL", 0xFF, false }, { "54.LFL", 0xFF, false },
    { "55.LFL", 0xFF, false }, { "56.LFL", 0xFF, false },
    { "57.LFL", 0xFF, false }, { "58.LFL", 0xFF, false },
    { "59.LFL", 0xFF, false }, { "60.LFL", 0xFF, false },
    { "61.LFL", 0xFF, false }, { "62.LFL", 0xFF, false },
    { "63.LFL", 0xFF, false },
    { nullptr,  0,    false },
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
