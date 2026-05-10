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
// Read-modify-write on the config blob — preserves text_scale_pct and
// use_mi_font.
void save_volume(int level);

// Speech text scale, percent of source-pixel size: 75..100 in steps of 5.
// 75 % is the original ThumbyScummby default that matches the scene-blit
// downsample; 100 % renders at full source resolution.
bool load_text_scale_pct(int *out_pct);
void save_text_scale_pct(int pct);

// Speech font selector.  When true, actor-talk / banner / modal speech
// renders via the MI overlay font instead of the SCUMM CHAR resource —
// pixel-perfect at LCD resolution, often more readable on the 128-px
// display than scaled-down SCUMM glyphs.
bool load_use_mi_font(bool *out);
void save_use_mi_font(bool v);

}  // namespace config_backend
}  // namespace tsb
