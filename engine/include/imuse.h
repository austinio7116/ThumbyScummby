// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — iMUSE sequencer (single-song slot).
//
// Plays SCUMM v4/v5 'RO'/'SO' (Roland MIDI / euphony) and 'AD' (AdLib)
// stream resources. Decodes VLQ deltas and dispatches MIDI events to the
// AdLib driver, with tempo (meta 0x51) and end-of-track (meta 0x2F)
// handling and AD-stream auto-loop. Music timer is exposed as
// `imuse_get_music_timer()` per upstream Player::getMusicTimer
// (scummvm-upstream/engines/scumm/imuse/imuse_player.cpp:133).
//
// Out of scope for MI1 boot: hook callbacks, scene transitions, loop
// conditionals, parameter faders, and the IMuseInternal::doCommand
// host-control interface — those are only used by later v5+ titles
// and would land as separate features rather than being approximated
// here.

#pragma once

#include "types.h"

namespace tsb {

// Initialize. No-op besides clearing internal state.
void imuse_init();

// Begin playing sound_id. sound_resource is the SOUN small-chunk PAYLOAD
// returned by tsb::resource_get_sound(). Empty span = no-op.
// Returns true if the resource was understood and playback started.
bool imuse_start_sound(int sound_id, Span sound_resource);

// Stop a specific sound; if the running song matches, halt playback.
void imuse_stop_sound(int sound_id);

// Stop everything.
void imuse_stop_all();

// True if sound_id is currently playing.
bool imuse_is_running(int sound_id);

// Advance the sequencer by elapsed_us microseconds, dispatching any due
// events. Called from the audio mix callback - we tick once per chunk.
void imuse_tick(uint32_t elapsed_us);

// Return current song's sound_id, or 0 if nothing playing. Useful for log.
int imuse_current_sound();

// Music timer used by VAR_MUSIC_TIMER (script-visible). Mirrors ScummVM
// Player::getMusicTimer (imuse_player.cpp:133): ticks*2/PPQN.
int imuse_get_music_timer();

}  // namespace tsb
