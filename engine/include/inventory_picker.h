// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — RB-tap inventory picker overlay.

#pragma once

namespace tsb {
class ScummEngine;

namespace inventory_picker {

// Run the picker overlay loop.  Reads `_verbs[]` for ImageVerb-type
// slots (those are the inventory item sprites the engine would have
// drawn into source rows ~177..199), shows their object names with
// the MI font, and synthesizes a click on the chosen item's curRect.
void run(ScummEngine *engine);

}  // namespace inventory_picker
}  // namespace tsb
