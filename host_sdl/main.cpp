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
#include "common/events.h"
#include "common/keyboard.h"
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

// SDL → Common::Event translator installed in OSystem_Thumby.  Pumps
// SDL_PollEvent and converts to scummvm Common::Event so the engine's
// EventManager (and downstream parseEvents / processInput / mouse
// handlers) see real input.
//
// Mouse coords are converted from window space → 320x200 game space,
// matching the active scale mode.  Currently we always use Fit
// (320x200 scaled to 128x80 letterboxed inside 128x128 logical), so
// invert that mapping here.
static bool sdl_to_scummvm_event(void * /*user*/, Common::Event *out) {
    SDL_Event ev;
    if (!SDL_PollEvent(&ev)) return false;
    out->type = Common::EVENT_INVALID;
    out->kbdRepeat = false;

    auto convertMouse = [&](int wx, int wy, Uint32 windowID) {
        // Resolve the window from the event's windowID — SDL_GetMouseFocus()
        // returned null when the window was unfocused or the cursor hadn't
        // entered it since startup, which made every click register at
        // (0, 0).  Using the windowID stored on the event is reliable.
        SDL_Window *w = SDL_GetWindowFromID(windowID);
        if (!w) {
            // Fallback: walk the SDL window list and grab the first one.
            // Our app only has one window.
            w = SDL_GetMouseFocus();
        }
        if (!w) {
            out->mouse.x = 0; out->mouse.y = 0; return;
        }
        int ww, wh;
        SDL_GetWindowSize(w, &ww, &wh);
        if (ww <= 0 || wh <= 0) { out->mouse.x = 0; out->mouse.y = 0; return; }
        // Logical 128x128 fills the window; Fit sub-region is the central
        // 128x80 band.  Map window x → game x ∈ [0,320), window y → game y
        // ∈ [0,200) using the Fit projection inverse.
        int lx = wx * 128 / ww;
        int ly = wy * 128 / wh;
        const int dst_h = 80;
        const int top   = (128 - dst_h) / 2; // 24
        int gx = lx * 320 / 128;
        int gy = (ly - top) * 200 / dst_h;
        if (gx < 0)   gx = 0;
        if (gx > 319) gx = 319;
        if (gy < 0)   gy = 0;
        if (gy > 199) gy = 199;
        out->mouse.x = gx;
        out->mouse.y = gy;
    };

    switch (ev.type) {
    case SDL_QUIT:
        out->type = Common::EVENT_QUIT;
        return true;
    case SDL_MOUSEMOTION:
        out->type = Common::EVENT_MOUSEMOVE;
        convertMouse(ev.motion.x, ev.motion.y, ev.motion.windowID);
        return true;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        const bool down = (ev.type == SDL_MOUSEBUTTONDOWN);
        if (ev.button.button == SDL_BUTTON_LEFT)
            out->type = down ? Common::EVENT_LBUTTONDOWN : Common::EVENT_LBUTTONUP;
        else if (ev.button.button == SDL_BUTTON_RIGHT)
            out->type = down ? Common::EVENT_RBUTTONDOWN : Common::EVENT_RBUTTONUP;
        else
            return false;
        convertMouse(ev.button.x, ev.button.y, ev.button.windowID);
        return true;
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        const bool down = (ev.type == SDL_KEYDOWN);
        out->type = down ? Common::EVENT_KEYDOWN : Common::EVENT_KEYUP;
        out->kbdRepeat = (ev.key.repeat != 0);
        // SDL_Keycode → Common::KeyCode is 1:1 for ASCII range.  Anything
        // we don't map cleanly we drop into the keycode unchanged; SCUMM
        // mostly checks ASCII chars and a few sentinels (ESC=27, F-keys).
        Common::KeyCode kc = (Common::KeyCode)ev.key.keysym.sym;
        out->kbd.keycode = kc;
        // ASCII: SDL gives the keycode; cast for letters/digits.  For
        // ESC/RETURN/SPACE the sym IS the ASCII code.
        if (ev.key.keysym.sym >= 0 && ev.key.keysym.sym < 128) {
            out->kbd.ascii = (uint16)ev.key.keysym.sym;
        } else {
            out->kbd.ascii = 0;
        }
        out->kbd.flags = 0;
        if (ev.key.keysym.mod & KMOD_SHIFT) out->kbd.flags |= Common::KBD_SHIFT;
        if (ev.key.keysym.mod & KMOD_CTRL)  out->kbd.flags |= Common::KBD_CTRL;
        if (ev.key.keysym.mod & KMOD_ALT)   out->kbd.flags |= Common::KBD_ALT;
        return true;
    }
    default:
        return false;
    }
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
    tsb::platform::checkpoint("after platform_sdl::init (SDL, win, audio_dev)", 0);

    if (!tsb::platform_sdl::load_data_dir(argv[1])) {
        fprintf(stderr, "failed to load game data from %s\n", argv[1]);
        tsb::platform_sdl::shutdown();
        return 1;
    }
    tsb::platform::checkpoint("after load_data_dir (LFL/LEC blobs in heap)", 0);

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
    tsb::platform::checkpoint("after audio init (opl2/adlib/imuse)", 0);

    // OSystem subclass that bridges to tsb::platform::*.  Lives on the stack
    // for the duration of main(); engine holds a pointer.
    tsb::OSystem_Thumby osys;
    osys.setEventPoller(sdl_to_scummvm_event, nullptr);
    osys.initBackend();
    tsb::platform::checkpoint("after OSystem initBackend", 0);
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
    fprintf(stderr, "[size] sizeof(ScummEngine_v4) = %zu KB (%zu bytes)\n",
            sizeof(tsb::ScummEngine_v4) / 1024, sizeof(tsb::ScummEngine_v4));
#define SZ(T) fprintf(stderr, "[size] %-40s = %6zu B\n", #T, sizeof(T))
    SZ(::Engine);
    SZ(tsb::ScummEngine);
    SZ(tsb::ScummEngine_v5);
    SZ(tsb::ScummEngine_v4);
    SZ(Graphics::Surface);
    SZ(Common::String);
    SZ(tsb::VirtScreen);
    SZ(tsb::ColorCycle);
    SZ(tsb::ScummFile);
    SZ(Common::ConfigManager);
    SZ(Common::EventDispatcher);
    SZ(Common::EventManager);
#undef SZ
    fprintf(stderr, "[size] ScummEngine vs sum-of-bigs:\n");
    fprintf(stderr, "  _grabbedCursor                  16384\n");
    fprintf(stderr, "  _NESPatTable                     8192\n");
    fprintf(stderr, "  _localScriptOffsets              4096\n");
    fprintf(stderr, "  gfxUsageBits                     4920\n");
    tsb::ScummEngine *eng = (dr.game.version == 4)
        ? (tsb::ScummEngine *)new tsb::ScummEngine_v4(&osys, dr)
        : (tsb::ScummEngine *)new tsb::ScummEngine_v5(&osys, dr);
    tsb::platform::checkpoint("after ScummEngine ctor", 0);

    Common::Error err = eng->run();
    if (err.getCode() != Common::kNoError) {
        fprintf(stderr, "engine run() returned error\n");
    }

    delete eng;
    tsb::platform_sdl::shutdown();
    return 0;
}
