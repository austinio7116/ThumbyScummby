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

}  // namespace tsb
