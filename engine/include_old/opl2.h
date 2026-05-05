// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — OPL2 (Yamaha YM3812) emulation API.
//
// Implementation lives in engine/src/opl2.cpp, which is a thin shim
// over the canonical DOSBox dbopl emulator (engine/src/dbopl.cpp +
// engine/include/dbopl.h, dropped in verbatim from
// scummvm-upstream/audio/softsynth/opl/dbopl.{cpp,h}, GPL-2.0+ © The
// DOSBox Team — see those files for original attribution).

#pragma once

#include <stdint.h>

namespace tsb {

// Initialize the chip. sample_rate is the desired output rate. Internal phase
// math is referenced to 49716 Hz and downsampled implicitly by frequency
// scaling. Safe to call multiple times - resets state.
void opl2_init(int sample_rate);

// Write to an OPL2 register (0x00 - 0xF5 range valid). Values outside the
// documented set are silently ignored.
void opl2_write_reg(uint8_t reg, uint8_t val);

// Read back a previously written value (some AdLib code does
// read-modify-write on B0 to honor the existing key-on bit).
uint8_t opl2_read_reg(uint8_t reg);

// Render n mono int16 samples. Samples are summed into out[] - caller must
// memset(out, 0, ...) first if they want pure OPL output.
void opl2_render_samples_add(int16_t *out, int n);

// Render n mono int16 samples, overwriting out[].
void opl2_render_samples(int16_t *out, int n);

}  // namespace tsb
