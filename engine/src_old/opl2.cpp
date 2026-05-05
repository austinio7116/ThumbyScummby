// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// OPL2 (Yamaha YM3812) emulation. Earlier we had a hand-rolled
// emulator here whose audit (H94) flagged structural defects: 16-entry
// envelope rate table vs the chip's 64, envelopes ticking at output
// sample rate instead of chip rate (notes ~6 cents flat), KSL stored
// but never applied, AM/vibrato ignored, wave 3 zero-shaped where it
// should be half-peaked.
//
// This file is now a thin shim around the canonical DOSBox dbopl
// emulator (engine/src/dbopl.cpp + engine/include/dbopl.h, dropped
// in verbatim from scummvm-upstream/audio/softsynth/opl/dbopl.{cpp,h}
// keeping their original Copyright (C) 2002-2011 The DOSBox Team
// header). The shim adapts our register-write + render-samples API
// surface to DBOPL::Chip so adlib.cpp and the audio mixer don't
// change.

#include "opl2.h"
#include "dbopl.h"

#include <stdint.h>

namespace tsb {

// dbopl lives in OPL::DOSBox::DBOPL. Pull only the types we need into
// scope rather than `using namespace …` (avoids polluting tsb).
using DbChip = ::OPL::DOSBox::DBOPL::Chip;
using DbBit32s = ::OPL::DOSBox::DBOPL::Bit32s;
using DbBit32u = ::OPL::DOSBox::DBOPL::Bit32u;
using DbBit8u  = ::OPL::DOSBox::DBOPL::Bit8u;
using DbBitu   = ::OPL::DOSBox::DBOPL::Bitu;

// One persistent chip instance. dbopl carries no global state beyond
// its lookup tables; the per-chip state lives entirely in this object.
static DbChip g_chip;

// dbopl's lookup tables (envelope rates, frequency multipliers, wave
// shapes, etc.) are filled lazily on first opl2_init. InitTables is
// idempotent; we still gate so we don't redo the work on a second init.
static bool g_tables_inited = false;

// Cached register values so opl2_read_reg can return what was last
// written. dbopl itself does not expose a register read-back path
// (the hardware doesn't either); a few iMUSE driver variants do
// read-modify-write on B0 to preserve key-on bits across pitch
// changes, so we shadow every write here.
static uint8_t g_reg_shadow[256] = {};

void opl2_init(int sample_rate) {
    if (!g_tables_inited) {
        ::OPL::DOSBox::DBOPL::InitTables();
        g_tables_inited = true;
    }
    // Reset chip state. `g_chip = DbChip();` materialises a ~4.4KB
    // Chip temporary on the stack — the default Pico SDK 2KB stack
    // accommodates the overflow into adjacent scratch_x harmlessly
    // (nothing else uses it on the single-core build). Required so a
    // re-init re-zeros chip state; on device the second opl2_init
    // call (when actual_rate != requested) is dead code anyway since
    // PWM is hardcoded to 22050.
    g_chip = DbChip();
    if (sample_rate <= 0) sample_rate = 22050;
    g_chip.Setup((DbBit32u)sample_rate);

    for (int i = 0; i < 256; i++) g_reg_shadow[i] = 0;
}

void opl2_write_reg(uint8_t reg, uint8_t val) {
    g_reg_shadow[reg] = val;
    g_chip.WriteReg((DbBit32u)reg, (DbBit8u)val);
}

uint8_t opl2_read_reg(uint8_t reg) {
    return g_reg_shadow[reg];
}

// dbopl produces 32-bit signed mono samples in a transient buffer.
// Saturate to int16. dbopl's full-mix peak is around ±2^14 so doubling
// before saturation lifts a normally-mixed track to full int16 range
// (loud peaks will clip to ±32767 — that's intentional, sounds louder
// at the cost of slight distortion on already-loud content; better
// trade-off on the Thumby's tiny PWM speaker than the previous
// half-loud passthrough).
constexpr int kOutputGain = 2;

static inline int16_t clip_to_int16(int32_t s) {
    if (s > 32767) return 32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}

static void render_into(int16_t *out, int n_samples, bool add) {
    if (n_samples <= 0) return;
    constexpr int kChunk = 256;
    DbBit32s buf[kChunk];
    while (n_samples > 0) {
        int n = n_samples < kChunk ? n_samples : kChunk;
        g_chip.GenerateBlock2((DbBitu)n, buf);
        for (int i = 0; i < n; i++) {
            int16_t v = clip_to_int16((int32_t)buf[i] * kOutputGain);
            out[i] = add ? clip_to_int16((int32_t)out[i] + v) : v;
        }
        out += n;
        n_samples -= n;
    }
}

void opl2_render_samples(int16_t *out, int n) {
    render_into(out, n, /*add=*/false);
}

void opl2_render_samples_add(int16_t *out, int n) {
    render_into(out, n, /*add=*/true);
}

}  // namespace tsb
