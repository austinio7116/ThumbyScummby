// ThumbyScummby — engine entry points.
//
// The platform layer provides startup data and pumps events; the engine
// runs the game.

#pragma once

#include "types.h"
#include "platform.h"

namespace tsb {

// Initialize the engine. Game data must already be installed via
// platform::data_*(). Returns true on success. After this returns, call
// engine_tick() once per frame.
bool engine_init();

// Run one frame: process input, advance scripts, render. Returns false to
// request a quit.
bool engine_tick();

// Optional: cleanly shut down. Free's nothing on device (no malloc).
void engine_shutdown();

}  // namespace tsb
