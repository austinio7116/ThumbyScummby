// ThumbyScummby — Pico SDK device entry point.
//
// Brings up clocks, peripherals (LCD, buttons, audio), then hands off to
// the engine_init() + engine_tick() loop. Game data is .incbin'd in
// flash; see data_section.S + tools/pack_device.py.

#include "engine.h"
#include "platform.h"

extern "C" {
#include "pico/stdlib.h"
#include "hardware/clocks.h"
}

namespace tsb::platform_pico {
    void init_all();
    bool blob_ok();
}

int main() {
    // 250 MHz: matches ThumbyDOOM/ThumbyNES baseline. We can push to 300
    // MHz later if profiling demands it.
    set_sys_clock_khz(250000, true);

    stdio_init_all();

    tsb::platform_pico::init_all();

    if (!tsb::platform_pico::blob_ok()) {
        // Without a valid data blob there's nothing to run. Sit and blink
        // a fault pattern via the audio enable LED-equivalent so the
        // dev knows pack_device.py wasn't run.
        while (1) {
            tsb::platform::sleep_ms(500);
        }
    }

    if (!tsb::engine_init()) {
        tsb::platform::log("engine_init failed\n");
        while (1) tsb::platform::sleep_ms(1000);
    }

    while (true) {
        if (!tsb::engine_tick()) break;
    }

    tsb::engine_shutdown();
    while (true) tsb::platform::sleep_ms(1000);
    return 0;
}
