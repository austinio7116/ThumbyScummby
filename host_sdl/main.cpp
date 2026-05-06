// ThumbyScummby — SDL host main (post-OSystem-pivot).
//
// Usage: thumbyscummby <path-to-mi1-data-dir>
//
// Lifecycle:
//   1. tsb::platform_sdl::init       — SDL2 window, audio, input
//   2. tsb::platform_sdl::load_data_dir — mmap + decrypt LFL/LEC chunks
//   3. tsb::imuse_init               — DOSBox OPL2 + iMUSE sequencer
//   4. construct OSystem_Thumby      — translates engine I/O to platform::*
//   5. construct ScummEngine_v5      — the canonical scummvm interpreter
//   6. eng->run()                    — blocking main loop (init + go)
//
// On quit, OSystem_Thumby::quit() flips a flag the engine's loop
// checks via Engine::shouldQuit().

#include "scummvm_compat.h"
#include "scumm/scumm.h"
#include "scumm/scumm_v5.h"
#include "scumm/detection.h"
#include "osystem_thumby.h"
#include "platform.h"
#include "imuse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

namespace tsb::platform_sdl {
    bool init(int argc, char **argv);
    bool main_loop_iter();
    void shutdown();
    bool load_data_dir(const char *path);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-mi1-data-dir>\n", argv[0]);
        fprintf(stderr, "  expects: 000.LFL, DISK01.LEC..DISK04.LEC, 901-904.LFL\n");
        return 1;
    }

    if (!tsb::platform_sdl::init(argc, argv)) {
        fprintf(stderr, "platform init failed\n");
        return 1;
    }

    if (!tsb::platform_sdl::load_data_dir(argv[1])) {
        fprintf(stderr, "failed to load game data from %s\n", argv[1]);
        tsb::platform_sdl::shutdown();
        return 1;
    }

    // iMUSE / OPL2 stack — driven by audio callback, hooked into platform.
    tsb::imuse_init();

    // OSystem subclass that bridges to tsb::platform::*.  Lives on the stack
    // for the duration of main(); engine holds a pointer.
    tsb::OSystem_Thumby osys;
    osys.initBackend();

    // Construct the canonical engine.  MI1 VGA Floppy is GID_MONKEY (v4)
    // — but our 100% transcribed runtime uses ScummEngine_v5 dispatch
    // (v5 is the dominant SCUMM5 variant; our MI1 floppy works in v5
    // codepath via DetectorResult.version=4 + DetectorResult.platform).
    tsb::DetectorResult dr;
    dr.game.id           = (int)tsb::GID_MONKEY;
    dr.game.version      = 4;
    dr.game.platform     = Common::kPlatformDOS;
    dr.game.features     = tsb::GF_SMALL_HEADER | tsb::GF_USE_KEY;
    dr.game.heversion    = 0;
    dr.language          = Common::EN_ANY;
    dr.extra             = "";
    // ScummEngine ctor reads dr.md5 as a 32-char hex string.  Use the
    // canonical MI1 VGA Floppy DOS English MD5.
    dr.md5               = "8e4ee4db46954bfcb6d2654dde0aae25";

    tsb::ScummEngine *eng = new tsb::ScummEngine_v5(&osys, dr);

    Common::Error err = eng->run();
    if (err.getCode() != Common::kNoError) {
        fprintf(stderr, "engine run() returned error\n");
    }

    delete eng;
    tsb::platform_sdl::shutdown();
    return 0;
}
