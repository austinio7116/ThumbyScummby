// Game descriptor table — one entry per supported SCUMM game.
//
// The platform layer scans /scumm/*/ at boot, matches against this
// table, and sets g_current_game.  main.cpp then asks the matched
// descriptor to construct the right ScummEngine subclass with the
// right DetectorResult.  Phase 1 of THUMBYONE_SLOT_PLAN: single slot
// binary serves every supported game, no more compile-time
// TSB_GAME_X selection.

#pragma once

// OSystem lives in the global namespace — scummvm_compat.h forward-
// declares it at top level, and the scumm_v* constructors take a
// global ::OSystem*.  ScummEngine lives in `namespace Scumm` (which
// scummvm_compat.h rewrites to tsb).
class OSystem;

namespace tsb {

class ScummEngine;

enum class ContainerVariant {
    V4_FLOPPY,   // 000.LFL + DISK01-04.LEC + 901-904.LFL  (MI1 VGA, etc)
    V5_HD,       // <hd_basename>.000 + <hd_basename>.001  (MI2, Indy4)
    V3_LFL,      // NN.LFL per room                        (Indy3 EGA — not yet)
};

// One expected file inside a game subdir.  The preload pipeline uses
// the XOR byte to decrypt raw LucasArts disks in-place; the resolver
// uses the filename to map engine requests onto platform Spans.
struct GameFile {
    const char *name;       // e.g. "DISK01.LEC" or "atlantis.001"
    uint8_t     xor_byte;   // 0 = already plain; 0x69 = LucasArts XOR
    bool        required;   // false = optional helper (e.g. 902.LFL)
};

struct GameDescriptor {
    const char       *subdir;          // /scumm/<subdir>/
    const char       *display_name;    // shown in picker
    ContainerVariant  variant;
    const char       *hd_basename;     // V5_HD only — null otherwise

    // Files expected inside the subdir.  Null-terminated array (last
    // entry has name=nullptr).  Preload uses xor_byte to decrypt raw
    // dumps in place; presence/required is used by the boot scan to
    // tell "ready" from "still missing files".
    const GameFile   *files;

    // Construct a ready-to-init engine instance (caller owns).
    // Implementation in game_table.cpp where DetectorResult /
    // ScummEngine_v* types are in scope.
    ScummEngine *(*create_engine)(::OSystem *osys);
};

extern const GameDescriptor kGameTable[];
extern const int            kGameTableCount;

// Currently-selected game.  Standalone builds pre-set via TSB_GAME_X
// at compile time; slot builds set this at boot after scanning the
// shared FAT.  Null means no game selected (boot scan found nothing).
extern const GameDescriptor *g_current_game;

}  // namespace tsb
