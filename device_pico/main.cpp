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

// Boot splashes — only ULTRA-distinct primaries so the user can name
// the colour without ambiguity.  Reads as a forward sequence:
//   RED → YELLOW → GREEN → CYAN → BLUE → MAGENTA → WHITE
// then the splashes inside ScummEngine::init() reuse the same palette
// (RED inside init = pre-setupScumm, YELLOW = post-setupScumm, etc).
//
// The colour you see when the device freezes is the LAST step we
// completed.

int main() {
    // 250 MHz: matches ThumbyDOOM/ThumbyNES baseline.
    set_sys_clock_khz(250000, true);
    stdio_init_all();

    tsb::platform_pico::init_all();

    if (!tsb::platform_pico::blob_ok()) {
        // No game data on flash — splash dark red and halt.
        tsb::platform::debug_splash(0x8000);
        while (1) tsb::platform::sleep_ms(500);
    }

    // Audio bring-up.
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

    static tsb::OSystem_Thumby osys;        // static — lives in BSS, not stack.
    osys.initBackend();
    extern OSystem *g_system;
    g_system = &osys;

    tsb::DetectorResult dr;
    dr.game.id        = (int)tsb::GID_MONKEY_VGA;
    dr.game.version   = 4;
    dr.game.platform  = Common::kPlatformDOS;
    dr.game.features  = tsb::GF_SMALL_HEADER | tsb::GF_USE_KEY;
    dr.game.heversion = 0;
    dr.language       = Common::EN_ANY;
    dr.extra          = "";
    dr.md5            = "8e4ee4db46954bfcb6d2654dde0aae25";

    tsb::ScummEngine *eng = new tsb::ScummEngine_v4(&osys, dr);

    Common::Error err = eng->init();
    if (err.getCode() == Common::kNoError) {
        err = eng->go();
    }
    (void)err;

    delete eng;
    while (true) tsb::platform::sleep_ms(1000);
    return 0;
}
