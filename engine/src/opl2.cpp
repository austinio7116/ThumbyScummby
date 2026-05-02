// ThumbyScummby - minimal OPL2 software emulator.
//
// Not a faithful dbopl port. Just enough to recognizably reproduce the
// adlib MIDI driver's register writes:
//
//   - 9 channels x 2 operators
//   - per-operator: phase accumulator, ADSR envelope, waveform select,
//     total-level (volume), key-scaling-rate, frequency multiplier,
//     attack/decay/sustain/release rates
//   - per-channel: FM (op1 modulates op2) or AM (sum), feedback (op1 modulates
//     itself N steps), block + fnum for pitch
//
// Reference: YMF262 / YM3812 application manual, Wohlstand's OPL2/OPL3
// emulator notes, and ScummVM's dbopl for cross-checking register layout.

#include "opl2.h"

#include <math.h>
#include <string.h>

namespace tsb {

// ---------- constants ----------
static constexpr int kNumChannels = 9;
static constexpr int kNumOperators = 18;

// Reference chip rate
static constexpr double kChipRate = 49716.0;

// Sine table: 1024 entries, 16-bit signed.
// Quarter-sine pattern stored at full precision; we mirror in lookup.
static int16_t s_sine[1024];

// Per-rate envelope step counters. Roughly matches OPL2 hardware decay table.
// We collapse the complex per-rate-keyscale stepping to a single per-tick
// linear envelope - imperfect but close enough that envelopes complete in
// recognizable time.
//
// We model envelopes in a 0..511 range (9-bit attenuation). 0 = full output,
// 511 = silence. Output amplitude is exp2(-att/8) approximately.

// Output scale - exp2 lookup so we can convert attenuation 0..511 to a linear
// gain. Each 8 attenuation steps = 1.5 dB roughly (matches OPL spec).
static int16_t s_exp[256];

// Operator state - 18 operators total.
struct Operator {
    // Phase accumulator (24-bit fixed point: high 10 bits = sine table index).
    uint32_t phase;
    // Phase increment per chip tick (24-bit fixed).
    uint32_t phase_inc;

    // Envelope state machine.
    enum Stage { ATTACK = 0, DECAY = 1, SUSTAIN = 2, RELEASE = 3, OFF = 4 };
    uint8_t  stage;
    uint16_t env_level;       // 0..511 (0 = full, 511 = silent)
    uint16_t sustain_level;   // 0..511

    // Rates (4-bit each, 0..15). Larger = faster.
    uint8_t  attack_rate;
    uint8_t  decay_rate;
    uint8_t  release_rate;

    // Total level (0..63) - directly added to envelope output (in 8x attenuation steps).
    uint8_t  total_level;

    // Key-scaling level (0..3) - reduces output for higher notes. Simplified.
    uint8_t  ksl;

    // Waveform select (0..3, only OPL2 ones).
    uint8_t  waveform;

    // Frequency multiplier (0..15).
    uint8_t  freq_mult;

    // KSR / EG-typ flags from 0x20 register.
    uint8_t  ksr;
    uint8_t  eg_type;     // 1 = sustained tone, 0 = percussive
    uint8_t  am;          // amplitude modulation (ignored in our minimal impl)
    uint8_t  vib;         // vibrato (ignored)

    // Last output sample (for feedback path on op1).
    int16_t  last_output;
};

struct Channel {
    Operator op[2];     // op[0] = modulator, op[1] = carrier

    // 10-bit fnum + 3-bit block from registers A0+n, B0+n.
    uint16_t fnum;
    uint8_t  block;
    uint8_t  key_on;

    // Feedback (0..7) + algorithm (0=FM, 1=AM) from C0+n.
    uint8_t  feedback;
    uint8_t  alg;

    // For feedback: average of last two op1 outputs.
    int16_t  fb_history[2];
};

struct State {
    int sample_rate;
    Channel ch[kNumChannels];
    uint8_t reg[256];

    // Output sample step relative to chip rate. Each call to render_one_sample
    // advances the chip by `chip_steps_per_output` chip cycles. We use
    // rational fixed-point: chip_step = sample_rate / kChipRate inverted, so
    // for each output sample we run kChipRate/sample_rate chip steps. We do
    // this with a simple counter and advance one chip step per output sample
    // because at 22050 Hz we under-sample (one output = ~2.25 chip ticks).
    // The phase increments are pre-scaled to match the output rate, so a
    // single phase advance per output sample gives the correct pitch.

    bool inited;
};

static State g{};

// ----- table builders -----
static void build_tables() {
    // Quarter-sine in [0, pi/2) with 256 points; mirror for full sine.
    for (int i = 0; i < 256; i++) {
        double angle = (double)i * (1.5707963267948966 / 256.0);
        double v = sin(angle);
        s_sine[i] = (int16_t)(v * 8191.0);              // [0..pi/2)
        s_sine[511 - i] = s_sine[i];                    // (pi/2..pi]
        s_sine[512 + i] = -s_sine[i];                   // (pi..3pi/2)
        s_sine[1023 - i] = -s_sine[i];                  // (3pi/2..2pi)
    }
    // exp table: index 0..255 maps to amplitude scaler. We use exp2(-i / 32)
    // so 256 steps = 8 octaves of attenuation.
    for (int i = 0; i < 256; i++) {
        double v = pow(2.0, -(double)i / 32.0);
        s_exp[i] = (int16_t)(v * 32767.0);
    }
}

// ----- helpers -----
static int16_t op_waveform(uint16_t phase_idx, uint8_t wave) {
    // phase_idx in 0..1023
    int16_t s = s_sine[phase_idx];
    switch (wave & 3) {
        case 0: return s;                                   // full sine
        case 1: return s < 0 ? 0 : s;                       // half sine (negative -> 0)
        case 2: return (s < 0) ? (int16_t)-s : s;           // abs sine
        case 3: {
            // OPL2 wave 3: only first quarter of each half cycle (0..255 and 512..767),
            // rest is 0.
            if (phase_idx < 256)        return s_sine[phase_idx];
            if (phase_idx < 512)        return 0;
            if (phase_idx < 768)        return s_sine[phase_idx - 512];
            return 0;
        }
    }
    return s;
}

// Attenuation -> linear sample. att unit = 1/32 octave (so 32 units = -6 dB).
// Range 0..255 covered by table directly. We accept att 0..1023 and just
// return 0 above 256 (~ -48 dB, effectively silent for our purposes).
static inline int32_t att_to_linear(int32_t att) {
    if (att <= 0) return 32767;
    if (att >= 256) return 0;
    return s_exp[att];
}

// Update phase increment for an operator based on its channel's fnum/block
// and the operator's freq_mult.
static void update_phase_inc(Operator &op, const Channel &ch, int sample_rate) {
    // OPL2 frequency formula: f_hz = fnum * 49716 / 2^20 * 2^block
    // Phase fixed-point uses 24-bit accumulator; high 10 bits index the sine.
    // phase_inc = freq_hz * (2^24) / sample_rate
    //           = (fnum << block) * 49716 * 16 / sample_rate
    //
    // Multiplier table: 2x scaled so 1 = 0.5x mult, 2 = 1.0x mult, etc.
    // We divide by 2 inside the formula.
    static const uint8_t mult[16] = {1,2,4,6,8,10,12,14,16,18,20,20,24,24,30,30};
    uint32_t base = (uint32_t)ch.fnum << ch.block;
    // base * 49716 * 8 * mult / sample_rate  (since 16/2 = 8)
    uint64_t num = (uint64_t)base * 49716ull * 8ull * (uint64_t)mult[op.freq_mult];
    if (sample_rate <= 0) sample_rate = 22050;
    op.phase_inc = (uint32_t)(num / (uint32_t)sample_rate);
}

// Build envelope step amount for current rate - "real" OPL2 has a complex
// table with 4 sub-rates per coarse rate; we approximate with a simple
// exponential schedule. Numbers are tuned so envelopes feel right.
static int env_step_for_rate(uint8_t rate) {
    // rate 0 = silent, 15 = ~instant (1 step ~= 8 atten units per sample).
    if (rate == 0) return 0;
    // step doubles every 4 rate units roughly.
    static const uint16_t table[16] = {
        0, 1, 1, 2, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 96, 256
    };
    return table[rate & 0xF];
}

// Tick one operator's envelope by one output sample.
static void tick_envelope(Operator &op) {
    switch (op.stage) {
        case Operator::ATTACK: {
            int step = env_step_for_rate(op.attack_rate);
            // Attack rolls atten level toward 0 (full). Use multiplicative falloff.
            if (step == 0) break;
            int new_lvl = op.env_level - step * 4;
            if (new_lvl <= 0) {
                op.env_level = 0;
                op.stage = Operator::DECAY;
            } else {
                op.env_level = (uint16_t)new_lvl;
            }
            break;
        }
        case Operator::DECAY: {
            int step = env_step_for_rate(op.decay_rate);
            int new_lvl = op.env_level + step;
            if (new_lvl >= op.sustain_level) {
                op.env_level = op.sustain_level;
                op.stage = Operator::SUSTAIN;
            } else {
                op.env_level = (uint16_t)new_lvl;
            }
            break;
        }
        case Operator::SUSTAIN:
            // Percussive (eg_type=0): keep decaying down to silence.
            if (!op.eg_type) {
                int step = env_step_for_rate(op.release_rate);
                int new_lvl = op.env_level + step;
                if (new_lvl >= 511) {
                    op.env_level = 511;
                    op.stage = Operator::OFF;
                } else {
                    op.env_level = (uint16_t)new_lvl;
                }
            }
            break;
        case Operator::RELEASE: {
            int step = env_step_for_rate(op.release_rate);
            int new_lvl = op.env_level + step;
            if (new_lvl >= 511) {
                op.env_level = 511;
                op.stage = Operator::OFF;
            } else {
                op.env_level = (uint16_t)new_lvl;
            }
            break;
        }
        default: break;
    }
}

// Compute one operator output sample.
// `mod_phase_offset` adds to phase index for FM modulation.
static int16_t compute_op(Operator &op, int32_t mod_phase_offset) {
    // Advance phase
    op.phase += op.phase_inc;
    uint16_t idx = (op.phase >> 14) & 0x3FF;        // top 10 bits
    int32_t phased = (int32_t)idx + mod_phase_offset;
    uint16_t phase_idx = (uint16_t)(phased & 0x3FF);

    int16_t wav = op_waveform(phase_idx, op.waveform);
    if (wav == 0) {
        op.last_output = 0;
        return 0;
    }

    // Apply envelope: total_level is added (in atten 0..63 doubled to 8x scale)
    int att = (int)op.env_level + ((int)op.total_level << 2);   // tl in 4x to align with env scale
    int32_t scale = att_to_linear(att);
    int32_t out = ((int32_t)wav * scale) >> 15;

    op.last_output = (int16_t)out;
    return (int16_t)out;
}

// Render one channel's combined sample.
static int32_t render_channel(Channel &ch) {
    // Tick envelopes once per sample.
    tick_envelope(ch.op[0]);
    tick_envelope(ch.op[1]);

    // Feedback: average of last two outputs of op[0] is fed back into op[0]
    // phase modulator. Strength controlled by feedback (0..7).
    int32_t fb_phase = 0;
    if (ch.feedback) {
        int32_t fb_avg = (ch.fb_history[0] + ch.fb_history[1]) >> 1;
        // Convert sample (-32768..32767) into a phase offset in 0..1023 range.
        // Strength: feedback=7 -> full ~±256 phase swing.
        int shift = 9 - ch.feedback;        // feedback=1 -> shift 8, =7 -> shift 2
        if (shift < 1) shift = 1;
        fb_phase = fb_avg >> shift;
    }

    int16_t op0 = compute_op(ch.op[0], fb_phase);
    ch.fb_history[1] = ch.fb_history[0];
    ch.fb_history[0] = op0;

    if (ch.alg == 0) {
        // FM: op0 modulates op1 phase.
        // Convert op0 sample to phase offset. Op output range is roughly
        // ±32767 - we scale down to ±512 phase units (half a cycle).
        int32_t mod = op0 >> 6;
        int16_t op1 = compute_op(ch.op[1], mod);
        return op1;
    } else {
        // AM: sum
        int16_t op1 = compute_op(ch.op[1], 0);
        return (int32_t)op0 + (int32_t)op1;
    }
}

// ---------- public API ----------

void opl2_init(int sample_rate) {
    static bool tables_built = false;
    if (!tables_built) {
        build_tables();
        tables_built = true;
    }
    memset(&g, 0, sizeof(g));
    g.sample_rate = sample_rate > 0 ? sample_rate : 22050;
    for (int i = 0; i < kNumChannels; i++) {
        g.ch[i].op[0].stage = Operator::OFF;
        g.ch[i].op[1].stage = Operator::OFF;
        g.ch[i].op[0].env_level = 511;
        g.ch[i].op[1].env_level = 511;
    }
    g.inited = true;
}

// Map register address to (channel, operator-within-channel) for operator-level
// regs (0x20-0x9F, 0xE0-0xEF). Returns -1 in op_out if address isn't valid.
static int reg_to_op_index(uint8_t reg, int *ch_out, int *op_idx_out) {
    // The operator slot offsets within a "register row":
    // slot 0..2 -> ch 0..2 op0
    // slot 3..5 -> ch 0..2 op1
    // slot 8..10 -> ch 3..5 op0
    // slot 11..13 -> ch 3..5 op1
    // slot 16..18 -> ch 6..8 op0
    // slot 19..21 -> ch 6..8 op1
    int slot = reg & 0x1F;
    if (slot >= 0x16) return -1;
    static const int8_t slot_to_ch[0x16] = {
        0, 1, 2,        // 0..2 ch0..2 mod
        3, 4, 5,        // 3..5 ch0..2 car
        -1, -1,         // 6,7 unused
        6, 7, 8,        // 8..10 ch3..5 mod
        9, 10, 11,      // 11..13 ch3..5 car
        -1, -1,         // 14,15 unused
        12, 13, 14,     // 16..18 ch6..8 mod
        15, 16, 17      // 19..21 ch6..8 car
    };
    int abs_op = slot_to_ch[slot];
    if (abs_op < 0) return -1;
    *ch_out = abs_op % 9;       // not quite right; build below
    // The "absolute operator" indexing: op_offset 0-2 = mod for ch0-2; 3-5 = car for ch0-2
    // Mapping abs_op (0..17) -> (ch, op_idx):
    int ch, op_idx;
    if (abs_op < 6) {
        op_idx = abs_op / 3;        // 0 = mod, 1 = car
        ch = abs_op % 3;
    } else if (abs_op < 12) {
        op_idx = (abs_op - 6) / 3;
        ch = 3 + (abs_op - 6) % 3;
    } else {
        op_idx = (abs_op - 12) / 3;
        ch = 6 + (abs_op - 12) % 3;
    }
    *ch_out = ch;
    *op_idx_out = op_idx;
    return 0;
}

void opl2_write_reg(uint8_t reg, uint8_t val) {
    g.reg[reg] = val;

    // Operator regs: 0x20-0x35, 0x40-0x55, 0x60-0x75, 0x80-0x95, 0xE0-0xF5
    if ((reg >= 0x20 && reg <= 0x35) ||
        (reg >= 0x40 && reg <= 0x55) ||
        (reg >= 0x60 && reg <= 0x75) ||
        (reg >= 0x80 && reg <= 0x95) ||
        (reg >= 0xE0 && reg <= 0xF5)) {
        int ch_i = -1, op_i = -1;
        if (reg_to_op_index(reg, &ch_i, &op_i) < 0) return;
        if (ch_i < 0 || ch_i >= 9 || op_i < 0 || op_i > 1) return;
        Operator &op = g.ch[ch_i].op[op_i];

        if (reg < 0x40) {
            // 0x20-0x35: am, vib, eg-type, ksr, freq-mult
            op.am = (val >> 7) & 1;
            op.vib = (val >> 6) & 1;
            op.eg_type = (val >> 5) & 1;
            op.ksr = (val >> 4) & 1;
            op.freq_mult = val & 0x0F;
            update_phase_inc(op, g.ch[ch_i], g.sample_rate);
        } else if (reg < 0x60) {
            // 0x40-0x55: ksl + total-level
            op.ksl = (val >> 6) & 3;
            op.total_level = val & 0x3F;
        } else if (reg < 0x80) {
            // 0x60-0x75: attack-rate (4) | decay-rate (4)
            op.attack_rate = (val >> 4) & 0x0F;
            op.decay_rate = val & 0x0F;
        } else if (reg < 0xA0) {
            // 0x80-0x95: sustain-level (4) | release-rate (4)
            uint8_t sl = (val >> 4) & 0x0F;
            // Sustain level: 0=loudest, 15=silence; convert to atten 0..480
            op.sustain_level = sl == 15 ? 480 : (sl * 32);
            op.release_rate = val & 0x0F;
        } else {
            // 0xE0-0xF5: waveform select
            op.waveform = val & 3;
        }
        return;
    }

    // Channel regs: 0xA0-0xA8 (fnum lo), 0xB0-0xB8 (fnum hi + block + key-on),
    // 0xC0-0xC8 (feedback + algorithm).
    if (reg >= 0xA0 && reg <= 0xA8) {
        int n = reg - 0xA0;
        Channel &ch = g.ch[n];
        ch.fnum = (ch.fnum & 0x300) | val;
        update_phase_inc(ch.op[0], ch, g.sample_rate);
        update_phase_inc(ch.op[1], ch, g.sample_rate);
        return;
    }
    if (reg >= 0xB0 && reg <= 0xB8) {
        int n = reg - 0xB0;
        Channel &ch = g.ch[n];
        ch.fnum = (ch.fnum & 0xFF) | ((uint16_t)(val & 3) << 8);
        ch.block = (val >> 2) & 7;
        uint8_t new_key = (val >> 5) & 1;
        if (new_key && !ch.key_on) {
            // Key-on: reset envelope to attack stage with full attenuation.
            for (int o = 0; o < 2; o++) {
                ch.op[o].stage = Operator::ATTACK;
                ch.op[o].env_level = 511;
                ch.op[o].phase = 0;
            }
        } else if (!new_key && ch.key_on) {
            // Key-off: jump to release.
            for (int o = 0; o < 2; o++) {
                if (ch.op[o].stage != Operator::OFF) {
                    ch.op[o].stage = Operator::RELEASE;
                }
            }
        }
        ch.key_on = new_key;
        update_phase_inc(ch.op[0], ch, g.sample_rate);
        update_phase_inc(ch.op[1], ch, g.sample_rate);
        return;
    }
    if (reg >= 0xC0 && reg <= 0xC8) {
        int n = reg - 0xC0;
        Channel &ch = g.ch[n];
        ch.feedback = (val >> 1) & 7;
        ch.alg = val & 1;
        return;
    }

    // 0x01 (test/wave-enable), 0x02-0x04 (timer), 0x08 (csm/keysplit),
    // 0xBD (rhythm), etc. - ignored in our minimal impl.
}

uint8_t opl2_read_reg(uint8_t reg) {
    return g.reg[reg];
}

void opl2_render_samples(int16_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int32_t sum = 0;
        for (int c = 0; c < kNumChannels; c++) {
            sum += render_channel(g.ch[c]);
        }
        // Clip
        if (sum > 32767) sum = 32767;
        else if (sum < -32768) sum = -32768;
        out[i] = (int16_t)sum;
    }
}

void opl2_render_samples_add(int16_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int32_t sum = out[i];
        for (int c = 0; c < kNumChannels; c++) {
            sum += render_channel(g.ch[c]);
        }
        if (sum > 32767) sum = 32767;
        else if (sum < -32768) sum = -32768;
        out[i] = (int16_t)sum;
    }
}

}  // namespace tsb
