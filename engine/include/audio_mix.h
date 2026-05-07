// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby - audio mix callback. Hooks into platform::audio_init.
//
// Per-callback work: tick iMUSE for the chunk's worth of time, then render
// OPL2 samples for the same chunk. SFX path is deferred (Phase 7).

#pragma once

#include <stdint.h>

namespace tsb {

// Configure the mixer. Called from engine_init AFTER opl2_init/adlib_init/imuse_init.
void audio_mix_init(int sample_rate);

// SDL/host callback. Mono int16, n samples.
void audio_mix_callback(void *user, int16_t *samples, int n_samples);

// Master volume on a 0..20 scale (default 10 = 100% unity gain).
//   level/10  is the linear gain applied after OPL2 render —
//   level=0   → silent, level=10 → unity, level=20 → 2× gain (with
//   per-sample int16 clamp).  Wired to the LB+UP/DOWN device chord.
void audio_mix_set_volume(int level_0_20);
int  audio_mix_get_volume();
constexpr int kAudioMixVolumeMax  = 20;
constexpr int kAudioMixVolumeUnit = 10;     // unity-gain step

}  // namespace tsb
