// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — resource lookup. Maps a (resource type, ID) pair to a
// span pointing into XIP-resident data. No copy.

#pragma once

#include "types.h"

namespace tsb {

// Initialize the resource subsystem after master_index is parsed and
// LOFF tables are resolved.
void resource_init();

// Look up resources by ID. Empty span means "not present" or "not loaded".
Span resource_get_room(int room_id);
Span resource_get_script(int script_id);
Span resource_get_costume(int costume_id);
Span resource_get_sound(int sound_id);

// LOAD/NUKE/LOCK/UNLOCK opcodes — for our XIP-resident model, locking is a
// no-op (resources are always accessible). Loading just verifies presence.
bool resource_load(int type, int id);   // 1=script,2=sound,3=costume,4=room

}  // namespace tsb
