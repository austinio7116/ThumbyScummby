// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — persistent config (volume + future settings).
//
// Single tiny blob.  Host: a text file alongside the data files.
// Device (RP2350): one 4 KB flash sector, separate from the game-save
// region so saving a game doesn't wipe settings.  Both backends expose
// the same load_volume / save_volume API.

#pragma once

namespace tsb {
namespace config_backend {

// Load the persisted master-volume level.  Returns true and writes the
// stored value into *out_level on success; false if no config exists or
// the blob is invalid (caller falls back to its compiled-in default).
// Output range: 0..kAudioMixVolumeMax (clamped here).
bool load_volume(int *out_level);

// Persist the master-volume level.  Best-effort: errors are silent —
// the user's runtime change still applies even if the write fails.
void save_volume(int level);

}  // namespace config_backend
}  // namespace tsb
