# iMUSE / AdLib OPL2 Spec for ThumbyScummby

> Distilled from `scummvm-upstream/{audio/adlib.cpp, audio/softsynth/opl/, engines/scumm/imuse/}`. Targets v5 floppy AdLib path only — no MT-32, no Roland, no CD audio.

## 1. SOUN resource (v5 floppy)

```
'SOUN'  size_BE
  uint8  ?       (mostly zero)
  uint8  ?
  uint16 first_subres_offs LE
  uint8  type   -- 1 = MIDI/iMUSE, 2 = CD, 3 = ROL, 4 = ADL, 5 = SBL, ...
  ... subresources, each with their own 4-byte tag and chunk-style header ...
```

For our v5 AdLib path we expect **type = 1** (or sometimes 4). The
subresource we want is the **AD** (or `ADL `) chunk. Other subresources
(`RO `, `SP `, etc.) are alternate driver formats — we ignore them.

### ADL subresource format

```
'AD' uint8 reserved   (or 'ADL ' on some games)
uint16 size_LE        (or BE depending; cross-reference at parse)
... AdLib MIDI-like event stream ...
```

The body is a custom AdLib MIDI stream:
- Standard MIDI status bytes (0x80-0xEF)
- Variable-length quantity (VLQ) delta times before each event
- iMUSE SysEx markers via `F0 7D <subcmd> ...`
- Meta events `FF nn ll ...` (tempo at 0x51, end-of-track at 0x2F)

For our minimal port: parse VLQ + MIDI events; treat SysEx as no-op (we
don't need transitions/hooks); honor tempo and end-of-track.

## 2. iMUSE sequencer minimum subset

We don't need full iMUSE. For "play a song from start to end" the engine
boils down to:

```cpp
struct ImuseSong {
    const uint8_t *event_stream;     // points into XIP'd ADL data
    uint32_t        next_event_offs;
    uint32_t        ticks_remaining;  // VLQ-decoded delta until next event
    uint32_t        tempo_us_per_tick;// from FF 51 03 ... default ~500us
    bool            playing;
    bool            looping;
};
```

```cpp
void imuse_tick(ImuseSong *song, uint32_t elapsed_us, AdlibState *adlib) {
    if (!song->playing) return;
    while (song->playing && elapsed_us >= song->ticks_remaining * song->tempo_us_per_tick) {
        elapsed_us -= song->ticks_remaining * song->tempo_us_per_tick;
        // dispatch event at current offset
        dispatch_midi_event(song, adlib);
        // read next VLQ delta
        song->ticks_remaining = read_vlq(song->event_stream, &song->next_event_offs);
    }
    song->ticks_remaining -= elapsed_us / song->tempo_us_per_tick;
}
```

That's the entire sequencer. The MIDI event dispatcher is below.

## 3. AdLib MIDI driver

### Per-MIDI-channel state

```cpp
struct AdlibChannel {
    int8_t   pitch_bend;          // -64..63 from MIDI pitch wheel
    uint8_t  volume;               // CC 7
    uint8_t  pan;                  // CC 10 (mostly ignored on OPL2)
    uint8_t  program;              // current GM instrument index
    AdlibVoice *voices[9];         // active voices on this channel (linked list)
    uint8_t  voice_count;
};
```

### Per-OPL voice state

```cpp
struct AdlibVoice {
    uint8_t  channel;              // owning MIDI channel (0..15)
    uint8_t  note;                 // MIDI note number (0..127)
    uint8_t  velocity;
    int8_t   priority;
    uint8_t  released;             // 0 = note-on; 1 = note-off pending release
    AdlibInstrument inst;          // 16-byte FM definition
};
```

### MIDI dispatch

```cpp
void midi_dispatch(uint8_t status, uint8_t d1, uint8_t d2, AdlibState *s) {
    uint8_t cmd = status & 0xF0;
    uint8_t ch  = status & 0x0F;
    switch (cmd) {
        case 0x90: // Note On (d2 = velocity; if 0, treat as Note Off)
            if (d2 == 0) note_off(s, ch, d1);
            else         note_on(s, ch, d1, d2);
            break;
        case 0x80: note_off(s, ch, d1); break;
        case 0xB0: control_change(s, ch, d1, d2); break;
        case 0xC0: program_change(s, ch, d1); break;
        case 0xE0: pitch_bend(s, ch, d1, d2); break;
        // 0xA0 (poly aftertouch), 0xD0 (channel pressure): ignore
        case 0xF0: meta_or_sysex(s, status, ...); break;
    }
}
```

### Note allocation

Voice picker is round-robin starting from voice 0. When all 9 are busy,
steal the oldest released voice; if none released, steal the lowest-priority
note. v5 MI1 typically uses 4-6 simultaneous notes, so the simple LRU
strategy works fine.

### MIDI Note → OPL2 frequency

```cpp
// Standard MIDI: A4 = 69 = 440 Hz
// OPL2 frequency = (block << 10) | fnum
// where freq_hz = (fnum * 49716 / 2^20) << block

static const uint16_t fnum_table[12] = {
    343, 363, 385, 408, 432, 458, 485, 514, 544, 577, 611, 647
};

void note_to_opl(int note, uint8_t *block_out, uint16_t *fnum_out) {
    int octave = (note / 12) - 1;
    int idx    = note % 12;
    if (octave < 0)  octave = 0;
    if (octave > 7)  octave = 7;
    *block_out = octave;
    *fnum_out  = fnum_table[idx];
}
```

For pitch bend, scale `fnum` by `2^(pitch_bend/8192*200_cents/1200_octave)`.

### OPL2 register-write helpers

```cpp
void opl_set_instrument(AdlibVoice *v, uint8_t opl_voice_n, OPL *opl) {
    // Voice n uses OPL operators (op_carrier, op_modulator).
    // For voices 0..8: op_modulator = voice_to_op[n][0], op_carrier = voice_to_op[n][1]
    static const uint8_t voice_to_op[9][2] = {
        {0,3},  {1,4},  {2,5},
        {8,11}, {9,12}, {10,13},
        {16,19},{17,20},{18,21}
    };
    uint8_t opmod = voice_to_op[opl_voice_n][0];
    uint8_t opcar = voice_to_op[opl_voice_n][1];

    opl->write_reg(0x20 + opmod, v->inst.mod_freq);
    opl->write_reg(0x40 + opmod, v->inst.mod_level);
    opl->write_reg(0x60 + opmod, v->inst.mod_attack_decay);
    opl->write_reg(0x80 + opmod, v->inst.mod_sustain_release);
    opl->write_reg(0xE0 + opmod, v->inst.mod_waveform);

    opl->write_reg(0x20 + opcar, v->inst.car_freq);
    opl->write_reg(0x40 + opcar, v->inst.car_level);
    opl->write_reg(0x60 + opcar, v->inst.car_attack_decay);
    opl->write_reg(0x80 + opcar, v->inst.car_sustain_release);
    opl->write_reg(0xE0 + opcar, v->inst.car_waveform);

    opl->write_reg(0xC0 + opl_voice_n, v->inst.feedback_alg);
}

void opl_note_on(AdlibVoice *v, uint8_t opl_voice_n, OPL *opl) {
    uint8_t  block;
    uint16_t fnum;
    note_to_opl(v->note + s->channels[v->channel].pitch_bend / 64, &block, &fnum);
    opl->write_reg(0xA0 + opl_voice_n, fnum & 0xFF);
    opl->write_reg(0xB0 + opl_voice_n, ((fnum >> 8) & 0x03) | (block << 2) | 0x20);  // KEY-ON
}

void opl_note_off(uint8_t opl_voice_n, OPL *opl) {
    // Clear bit 5 of B0+n; release envelope plays out
    uint8_t prev = opl_get_reg(0xB0 + opl_voice_n);
    opl->write_reg(0xB0 + opl_voice_n, prev & ~0x20);
}
```

### GM instrument table

We need a 128-entry hardcoded table mapping MIDI program numbers to AdLib
FM definitions. ScummVM's `audio/adlib.cpp:328+` has it as `g_gmInstruments[128]`.

We'll port that table verbatim. Each entry is ~16 bytes. Total ROM = 2 KB.

## 4. OPL2 emulator

We need a small OPL2 implementation. Two options:

### Option A: Port ScummVM's dbopl

Source: `audio/softsynth/opl/dbopl.{h,cpp}`. ~2,500 LOC of dense C++. Fair
fidelity, well-tested. Used by all ScummVM SCUMM games. State size ~4 KB.
Generates samples at native 49716 Hz; needs downsampling to 22050 Hz.

### Option B: Write minimal OPL2 from scratch

OPL2 is conceptually simple:
- 9 voices × 2 operators per voice = 18 operators
- Each operator = sine generator + envelope + amplitude scaler
- Voice algorithms: AM (sum) or FM (modulator drives carrier phase)
- Sine table: 1024 entries × 16-bit = 2 KB
- Logarithmic exp table: 256 entries × 16-bit = 512 bytes
- Per-operator state: phase, envelope counter, current level → ~32 bytes × 18 = 576 bytes
- Per-voice state: ~16 bytes × 9 = 144 bytes
- Total: ~3 KB code + 4 KB tables + 1 KB state = **~8 KB**

For Phase 6 we'd start with porting dbopl since it's known-working. If it's
too heavy on M33 we'd rewrite minimal version later.

## 5. Output mixing

For host SDL: the SDL audio callback fires every ~10ms. Each callback:

```cpp
void audio_callback(void *userdata, uint8_t *out_buf, int len) {
    int16_t *samples = (int16_t *)out_buf;
    int n = len / 2;  // mono int16
    for (int i = 0; i < n; i++) {
        // tick iMUSE sequencer once per sample-time
        imuse_tick(...);
        samples[i] = opl_render_sample();
    }
}
```

For device: we run OPL emulation on core1 driving PWM/DMA output, exactly
like ThumbyDOOM. iMUSE timer alarm posts events into a SPSC ring buffer
that core1 consumes between samples.

## 6. SFX path (separate from music)

SFX in v5 floppy live in **SBL** subresources of SOUN. They're VOC-format
8-bit signed PCM (8-11 kHz typically). Decode and play through a separate
mixer slot — **not** through the OPL2 path. We need:

```cpp
struct SfxSlot {
    const uint8_t *pcm;       // 8-bit signed
    uint32_t        length;
    uint32_t        offset;
    uint32_t        sample_rate;
    uint8_t         volume;
    bool            playing;
};
```

Mix down to int16 and add to the music output:
```cpp
final_sample = clamp(music_sample + sfx_sample * volume / 256, -32768, 32767);
```

## 7. Memory floor

| Component | KB |
|---|---:|
| iMUSE sequencer (single song state) | 0.5 |
| AdLib channels (16) + voices (9) | 1 |
| GM instrument table (128 × 16) | 2 |
| OPL2 emulator state (dbopl-class) | 4 |
| OPL2 sine + exp tables (ROM/flash) | 2.5 |
| Audio output buffer (4 KB) | 4 |
| SFX slots (4 × ~32 B) | 0.2 |
| **Total RAM** | **~10 KB** |
| **Total flash** | **~2.5 KB tables + ~30 KB code** |

Comfortably small. Audio is the cheapest subsystem in our budget.

## 8. Reference citations

| Topic | File | Lines |
|---|---|---|
| iMUSE Internal | `engines/scumm/imuse/imuse_internal.h` | 404-470 |
| Player::send | `engines/scumm/imuse/imuse_player.cpp` | 281-310 |
| AdLib driver | `audio/adlib.cpp` | 80-3000 |
| AdLibInstrument | `audio/adlib.cpp` | 56-78 |
| g_gmInstruments | `audio/adlib.cpp` | 328+ |
| dbopl emulator | `audio/softsynth/opl/dbopl.{h,cpp}` | full file |
| Sound::startSound | `engines/scumm/sound.cpp` | 120-150 |
| SOUN parse | `engines/scumm/sound.cpp` | 316-335 |
