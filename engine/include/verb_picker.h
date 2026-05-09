// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — LB-tap verb picker overlay.
//
// Replaces the on-screen verb panel.  The engine still renders verbs
// into source rows 144..199 of the framebuffer but we hide those rows;
// instead this overlay reads `_verbs[]` directly, lets the player pick
// one with the D-pad, and synthesizes a left-click at the verb's
// curRect to feed the SCUMM engine an event that's identical to a
// physical click on the verb panel.
//
// Auto-detect for dialog mode: when verb slots populate with the
// classic "talkable choice" pattern (high IDs + non-zero hicolor),
// run() called repeatedly with `auto_dialog_mode=true` will skip the
// LB-tap-to-open semantic — it's already open.

#pragma once

namespace tsb {
class ScummEngine;

namespace verb_picker {

// True if the engine's verb slots currently look like dialog options
// (high verbid + hicolor markings that the engine uses for dialog
// "talk to" replies).
bool dialog_mode_active(ScummEngine *engine);

// Run the picker overlay loop.  Refreshes the scene snapshot each
// frame; paints a translucent menu box; reads D-pad + A/B; on A
// synthesizes a click on the chosen verb and returns.
void run(ScummEngine *engine);

// The text of the verb most recently picked.  Empty until the user
// has used the verb picker once.  inventory_picker uses this to
// compose "<verb> <item>" on the LCD sentence strip.
const char *last_picked_verb_name();

}  // namespace verb_picker
}  // namespace tsb
