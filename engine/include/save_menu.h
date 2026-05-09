// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — minimal hold-LB save/load menu.
//
// Triggered when the LB shoulder button is held for ~800 ms while the
// player has control (`_userPut > 0`).  Pauses the engine, paints a small
// SAVE / LOAD / CANCEL menu directly onto the LCD framebuffer, blocks
// until the user picks an option.
//
// Single slot: the device backs onto the top 64 KB of flash; the host
// SDL build writes a single file in the cwd.  See save_backend.cpp.

#pragma once

namespace tsb {

class ScummEngine;

namespace save_menu {

// Should the menu open this frame?  Tracks LB hold time across calls;
// returns true exactly once when the threshold is crossed.  The caller
// (main.cpp) then runs the menu and re-enters the engine afterwards.
//
// `is_player_in_control` should be true only when `_userPut > 0` (no
// active cutscene).  When false, the hold timer resets so a long press
// during a cutscene doesn't fire the menu the moment control returns.
bool maybe_trigger(bool lb_held, bool is_player_in_control);

// Run the menu loop.  Polls platform input, paints the menu, dispatches
// SAVE / LOAD via the engine.  Returns when the user selects or cancels.
// `engine` must be non-null and at a savable point (player in control).
void run(ScummEngine *engine);

}  // namespace save_menu
}  // namespace tsb
