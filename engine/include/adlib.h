// ThumbyScummby - AdLib MIDI driver. Translates GM MIDI events into
// OPL2 register writes via the opl2_* API.
//
// Per the spec_04 reference: 16 MIDI channels (with channel 9 being
// percussion in GM, but iMUSE for MI1 doesn't really use percussion in
// the title music), 9 OPL2 voices, GM instrument table sized for 128
// programs.

#pragma once

#include <stdint.h>

namespace tsb {

// Initialize the driver. Resets channel state, allocates voices.
void adlib_init();

// Dispatch a MIDI event. status is the MIDI status byte (0x80-0xEF for
// channel messages, 0xF0-0xFF for system / meta - those last are
// ignored here; the iMUSE parser extracts tempo / EOT before calling us).
void adlib_midi_event(uint8_t status, uint8_t d1, uint8_t d2);

// Stop all voices (note-off all). Used by imuse_stop_all.
void adlib_silence_all();

// Bind a custom 11-byte AdLib instrument definition to a MIDI channel
// (0..15). When bound, future note-ons on that channel use this instrument
// instead of looking up GM_INSTRUMENTS[program]. Pass nullptr to clear.
//
// Layout of `def_11` (matches scummvm-upstream/audio/adlib.cpp AdLibInstrument):
//   [0]  modCharacteristic
//   [1]  modScalingOutputLevel
//   [2]  modAttackDecay
//   [3]  modSustainRelease
//   [4]  modWaveformSelect
//   [5]  carCharacteristic
//   [6]  carScalingOutputLevel
//   [7]  carAttackDecay
//   [8]  carSustainRelease
//   [9]  carWaveformSelect
//   [10] feedback
//
// Note: attack/decay and sustain/release are stored in their NATURAL form
// (the low-level driver bitwise-NOTs them when programming the OPL2 reg).
void adlib_set_channel_instrument(uint8_t midi_ch, const uint8_t *def_11);

// Clear all per-channel custom instrument overrides, returning channels
// to GM lookup mode. Called by imuse_start_sound when starting a new song.
void adlib_clear_channel_instruments();

}  // namespace tsb
