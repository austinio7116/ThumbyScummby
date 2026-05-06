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
#include "scumm/scumm_v4.h"
#include "scumm/detection.h"
#include "osystem_thumby.h"
#include "platform.h"
#include "imuse.h"
#include "audio_mix.h"
#include "opl2.h"
#include "adlib.h"

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

    // iMUSE / OPL2 stack — driven by SDL audio callback. The pre-pivot
    // engine.cpp wired this up; after the pivot we wire it here in main
    // so OPL2 and iMUSE are alive before any sound resource is started
    // by the engine.
    // Audio init order matches pre-pivot engine.cpp:1331-1349:
    //   opl2_init -> adlib_init -> imuse_init -> platform::audio_init
    // -> audio_mix_init, then re-init opl2/adlib if SDL gave us a
    // different rate.
    constexpr int kRequestedRate = 22050;
    tsb::opl2_init(kRequestedRate);
    tsb::adlib_init();
    tsb::imuse_init();
    int actual_rate = tsb::platform::audio_init(kRequestedRate,
                                                tsb::audio_mix_callback,
                                                nullptr);
    if (actual_rate <= 0) {
        tsb::platform::log("audio: platform::audio_init failed; running silent\n");
        actual_rate = kRequestedRate;
    } else {
        tsb::audio_mix_init(actual_rate);
        if (actual_rate != kRequestedRate) {
            tsb::opl2_init(actual_rate);
            tsb::adlib_init();
        }
        tsb::platform::log("audio: %d Hz mono\n", actual_rate);
    }

    // OSystem subclass that bridges to tsb::platform::*.  Lives on the stack
    // for the duration of main(); engine holds a pointer.
    tsb::OSystem_Thumby osys;
    osys.initBackend();
    extern OSystem *g_system;
    g_system = &osys;

    // Construct the canonical engine.  MI1 VGA Floppy is GID_MONKEY (v4)
    // — but our 100% transcribed runtime uses ScummEngine_v5 dispatch
    // (v5 is the dominant SCUMM5 variant; our MI1 floppy works in v5
    // codepath via DetectorResult.version=4 + DetectorResult.platform).
    tsb::DetectorResult dr;
    // GID_MONKEY_VGA is the floppy v4 (with copy-protection screen).
    // GID_MONKEY is the v5 CD release.  Setting v4=GID_MONKEY made
    // scummvm-upstream's copy-protection bypass workaround
    // (script_v5.cpp:2964 — `if (_game.id == GID_MONKEY_VGA && script == 152) return;`)
    // not fire, so script 152 ran and the boot got stuck waiting for a
    // dial-code answer the engine has no way to provide.
    dr.game.id           = (int)tsb::GID_MONKEY_VGA;
    dr.game.version      = 4;
    dr.game.platform     = Common::kPlatformDOS;
    dr.game.features     = tsb::GF_SMALL_HEADER | tsb::GF_USE_KEY;
    dr.game.heversion    = 0;
    dr.language          = Common::EN_ANY;
    dr.extra             = "";
    // ScummEngine ctor reads dr.md5 as a 32-char hex string.  Use the
    // canonical MI1 VGA Floppy DOS English MD5.
    dr.md5               = "8e4ee4db46954bfcb6d2654dde0aae25";

    // ScummVM hierarchy: ScummEngine_v4 inherits ScummEngine_v5
    // (older > newer numbering by inheritance).  For MI1 floppy we
    // instantiate v4 to get its readIndexFile / charset / decoder
    // overrides.  v5 codepaths (FOA, MI2) would use ScummEngine_v5.
    //
    // The user requested support for v5 too — both classes are
    // compiled and addressed; pick by dr.game.version.
    tsb::ScummEngine *eng = (dr.game.version == 4)
        ? (tsb::ScummEngine *)new tsb::ScummEngine_v4(&osys, dr)
        : (tsb::ScummEngine *)new tsb::ScummEngine_v5(&osys, dr);

    Common::Error err = eng->run();
    if (err.getCode() != Common::kNoError) {
        fprintf(stderr, "engine run() returned error\n");
    }

    delete eng;
    tsb::platform_sdl::shutdown();
    return 0;
}
