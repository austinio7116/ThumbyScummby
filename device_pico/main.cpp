// ThumbyScummby — Pico SDK device entry point.
//
// Brings up clocks, peripherals (LCD, buttons, audio), then constructs
// OSystem_Thumby + ScummEngine_v4 and hands off to eng->run().  Game
// data is .incbin'd in flash; see data_section.S + tools/pack_device.py.

#include "scummvm_compat.h"
#include "scumm/scumm.h"
#include "scumm/scumm_v4.h"
#include "scumm/detection.h"
#include "common/events.h"
#include "osystem_thumby.h"
#include "platform.h"
#include "imuse.h"
#include "audio_mix.h"
#include "opl2.h"
#include "adlib.h"

extern "C" {
#include "pico/stdlib.h"
#include "hardware/clocks.h"
}

namespace tsb::platform_pico {
    void init_all();
    bool blob_ok();
}

int main() {
    // 250 MHz: matches ThumbyDOOM/ThumbyNES baseline.  We can push to 300
    // MHz later if profiling demands it.
    set_sys_clock_khz(250000, true);

    stdio_init_all();

    tsb::platform_pico::init_all();

    if (!tsb::platform_pico::blob_ok()) {
        // Without a valid data blob there's nothing to run.  Sit and spin
        // so the dev knows pack_device.py wasn't run.
        while (1) {
            tsb::platform::sleep_ms(500);
        }
    }

    // -----------------------------------------------------------------
    // Audio init — match host_sdl/main.cpp order:
    //   opl2_init -> adlib_init -> imuse_init -> platform::audio_init
    //   -> audio_mix_init.  After audio_init the IRQ starts draining
    //   the PWM ring at 22050 Hz.
    // -----------------------------------------------------------------
    constexpr int kRequestedRate = 22050;
    tsb::opl2_init(kRequestedRate);
    tsb::adlib_init();
    tsb::imuse_init();
    int actual_rate = tsb::platform::audio_init(kRequestedRate,
                                                tsb::audio_mix_callback,
                                                nullptr);
    if (actual_rate <= 0) {
        actual_rate = kRequestedRate;
    } else {
        tsb::audio_mix_init(actual_rate);
        if (actual_rate != kRequestedRate) {
            tsb::opl2_init(actual_rate);
            tsb::adlib_init();
        }
    }

    // -----------------------------------------------------------------
    // OSystem + engine.
    // -----------------------------------------------------------------
    static tsb::OSystem_Thumby osys;        // static so it's not on the
                                            // limited cortex-m33 stack
    osys.initBackend();
    extern OSystem *g_system;
    g_system = &osys;

    tsb::DetectorResult dr;
    dr.game.id        = (int)tsb::GID_MONKEY_VGA;  // floppy v4
    dr.game.version   = 4;
    dr.game.platform  = Common::kPlatformDOS;
    dr.game.features  = tsb::GF_SMALL_HEADER | tsb::GF_USE_KEY;
    dr.game.heversion = 0;
    dr.language       = Common::EN_ANY;
    dr.extra          = "";
    dr.md5            = "8e4ee4db46954bfcb6d2654dde0aae25";

    tsb::ScummEngine *eng = new tsb::ScummEngine_v4(&osys, dr);

    Common::Error err = eng->run();
    (void)err;

    delete eng;
    while (true) tsb::platform::sleep_ms(1000);
    return 0;
}
