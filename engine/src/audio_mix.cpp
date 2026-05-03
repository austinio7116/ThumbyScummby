// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby - audio mix callback.

#include "audio_mix.h"
#include "imuse.h"
#include "opl2.h"

#include <string.h>

namespace tsb {

static int s_sample_rate = 22050;

void audio_mix_init(int sample_rate) {
    s_sample_rate = sample_rate > 0 ? sample_rate : 22050;
}

void audio_mix_callback(void * /*user*/, int16_t *samples, int n_samples) {
    // 1) Tick iMUSE for this chunk's worth of time.
    //    elapsed_us = n_samples / sample_rate * 1e6
    uint32_t elapsed_us = (uint32_t)((uint64_t)n_samples * 1000000ull / (uint64_t)s_sample_rate);
    imuse_tick(elapsed_us);

    // 2) Render OPL2 samples (overwriting buffer).
    opl2_render_samples(samples, n_samples);

    // 3) (Deferred) SFX add-mix would go here.
}

}  // namespace tsb
