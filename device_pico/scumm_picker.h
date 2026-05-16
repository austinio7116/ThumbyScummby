// ThumbyScummby SCUMM-slot picker — runs as the first phase of the
// SCUMM slot's main() before engine init.
//
// Launch model (P8-cart-style separate-boot for hygiene):
//
//   1. main() calls scumm_picker_consume_active_game().  If a prior
//      picker run wrote /scumm/.active_game, that file is consumed
//      here and g_current_game is set; the function returns true and
//      main() skips the picker UI entirely on this boot.
//   2. Otherwise main() calls scumm_picker_run().  On launch (A) the
//      picker writes /scumm/.active_game, then calls
//      thumbyone_handoff_request_slot() to reboot — the picker does
//      NOT return through that path.  On the *next* boot, step 1
//      consumes the file and we land in the engine with a virgin
//      heap.
//
// Why the reboot: ScummVM's heap usage is tight enough that the
// engine getting a freshly-zeroed heap (rather than one with picker
// allocations fragmented in) is worth the ~1.5 s extra boot delay.
// Mirrors the P8 cart-launch pattern (p8_relaunch_self).
//
// MENU-long-hold inside the picker reboots back to the ThumbyOne
// lobby via thumbyone_handoff_request_lobby — also does not return.

#pragma once

extern "C" {

// Check for a pending picker choice from a prior boot.  If
// /scumm/.active_game is present, read it, look up the matching
// descriptor in kGameTable, set tsb::g_current_game, delete the
// file, and return true.  Otherwise return false.  Idempotent —
// safe to call once per boot.
bool scumm_picker_consume_active_game(void);

// Run the picker UI.  On A-launch: writes /scumm/.active_game and
// reboots into the SCUMM slot — does NOT return.  On MENU-long-hold:
// reboots into the lobby — does NOT return.  Returns -1 only on
// unrecoverable setup failure (e.g. heap alloc failed).
int scumm_picker_run(void);

}  // extern "C"
