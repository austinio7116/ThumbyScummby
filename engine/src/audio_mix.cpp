// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby - audio mix callback.

#include "audio_mix.h"
#include "imuse.h"
#include "opl2.h"
#include "platform.h"

#include <string.h>

namespace tsb {

static int s_sample_rate = 22050;
static int s_volume      = 10;            // 0..kAudioMixVolumeMax

void audio_mix_init(int sample_rate) {
    s_sample_rate = sample_rate > 0 ? sample_rate : 22050;
}

void audio_mix_set_volume(int level) {
    if (level < 0) level = 0;
    if (level > kAudioMixVolumeMax) level = kAudioMixVolumeMax;
    s_volume = level;
}

int audio_mix_get_volume() {
    return s_volume;
}

void audio_mix_callback(void * /*user*/, int16_t *samples, int n_samples) {
    // 1) Tick iMUSE for this chunk's worth of time.
    //    elapsed_us = n_samples / sample_rate * 1e6
    uint32_t elapsed_us = (uint32_t)((uint64_t)n_samples * 1000000ull / (uint64_t)s_sample_rate);

    imuse_tick(elapsed_us);

    // 2) Render OPL2 samples (overwriting buffer).
    opl2_render_samples(samples, n_samples);

    // 3) Apply master volume.  level=10 is unity (skip the loop).  Below
    // unity attenuates, above unity boosts with int16 clamp on each
    // sample to avoid wraparound.
    const int v = s_volume;
    if (v == kAudioMixVolumeUnit) return;
    if (v == 0) {
        memset(samples, 0, (size_t)n_samples * sizeof(int16_t));
        return;
    }
    for (int i = 0; i < n_samples; i++) {
        int32_t scaled = (int32_t)samples[i] * v / kAudioMixVolumeUnit;
        if (scaled >  32767) scaled =  32767;
        if (scaled < -32768) scaled = -32768;
        samples[i] = (int16_t)scaled;
    }
}

}  // namespace tsb
