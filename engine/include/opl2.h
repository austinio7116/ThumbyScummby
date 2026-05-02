// ThumbyScummby - minimal OPL2 (Yamaha YM3812) software emulator.
//
// Not a perfect dbopl port - we hand-write a lean OPL2 in pure portable C++
// so it cross-compiles to ARM Cortex-M33 with no x86-isms. Goals:
//
//   - 9 channels x 2 operators FM synthesis
//   - ADSR envelope per operator
//   - Sine, half-sine, abs-sine, quarter-sine waveforms (OPL2 supports 4)
//   - Feedback path on operator 1 of each channel
//   - FM (modulator drives carrier phase) or AM (sum) per channel
//   - 22050 Hz output, mono int16
//
// Register-write semantics match the real chip's external interface so the
// AdLib MIDI driver code talks to it the same way it would talk to dbopl
// or a real OPL2 chip.

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
