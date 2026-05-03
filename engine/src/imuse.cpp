// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — iMUSE sequencer (single-song slot).
//
// Three stream formats supported, autodetected:
//
//   'RO' / 'SO' header (MI1/MI2 floppy Roland MT-32)
//      - 2 magic bytes 'R','O' or 'S','O'
//      - body = stream of events:
//          0xF0 <delay>      : add <delay> ticks to next event
//          0xF1 <delay>      : add 256 + <delay> ticks (rare)
//          status d1 d2      : standard MIDI 0x8/0x9/0xB events
//          status d1         : 0xC (program change) or 0xD (channel pressure)
//          0xA0+ch d1 d2     : marker (ignored)
//          0xFF              : autoloop -> end of track (treat as halt)
//          0x00              : end of track
//      - PPQN = 120, default tempo 500000us/quarter (so ~4167 us per tick).
//
//   'MThd' SMF header
//      - Standard MIDI File. We parse only ONE track (the first one),
//        ignore the others. VLQ delta times + MIDI events. 0xFF meta
//        events for tempo (0x51) and EOT (0x2F).
//
//   'AD' (AdLib) sub-chunk inside SO container - MI1 VGA floppy DOS uses
//   this for AdLib music. Layout (after the 6-byte SO and 6-byte AD small-
//   chunk headers):
//      [0..1]    header (a 2-byte version stamp; usually 0x7F 0x00; ignored)
//      [2]       0x80 = music, otherwise SFX
//      [3]       'ticks' (used to compute tempo per scummvm convertADResource)
//      [4]       play_once (1 = play once, 0 = loop)
//      [5..7]    reserved / unused
//      [8]       num_instr (typically 8)
//      [9..16]   channel array (1-based MIDI channel for each instrument)
//      [17..]    8 * 16 bytes instrument table
//      [17+128.. = 145..end]  raw MIDI track (status + params, with
//                              VLQ-encoded delta BEFORE every event after
//                              the first; the first event omits its delta).
//
// Of the 14 bytes per instrument, bytes [3..12] map to mod_*/car_* OPL2
// register values (matching scummvm convertADResource's bit layout) and
// byte [2] is the FM feedback / connection. We bind these to the AdLib
// driver as per-channel custom instruments via adlib_set_channel_instrument().
//
// SCUMM v4 floppy SOUN resources wrap one or more SO chunks. Each SO body
// holds nested 'WA' (Waveform / PC speaker / Roland MT-32) and 'AD'
// (AdLib FM) sub-chunks. We pick AD for AdLib output. WA support is not
// implemented - the platform target is OPL2-only.

#include "imuse.h"
#include "adlib.h"
#include "platform.h"

#include <string.h>
#include <stdio.h>
#ifndef THUMBY_DEVICE
#include <stdlib.h>     // getenv, atoi
#endif

namespace tsb {

// Default values
static constexpr uint32_t kDefaultTempoUsPerQuarter = 500000;   // 120 BPM
static constexpr uint16_t kRoPpqn = 120;
static constexpr uint32_t kRoTempoUsPerTick = 500000 / 120;

enum ParserType : uint8_t {
    PT_NONE = 0,
    PT_RO   = 1,        // RO / SO format (Roland MT-32 in MI1/MI2 floppy)
    PT_SMF  = 2,        // Standard MIDI file
    PT_AD   = 3,        // 'AD' AdLib sub-chunk (MI1/Indy3 v4 floppy AdLib)
};

struct PendingEvent {
    bool     valid;
    uint32_t delta_ticks;       // ticks-before-this-event
    uint8_t  status;
    uint8_t  d1, d2;
    // Meta event data (SMF only): 0x51 tempo, 0x2F EOT.
    uint8_t  meta_type;
    uint32_t meta_tempo_us_per_quarter;
};

struct Song {
    bool        playing;
    int         sound_id;

    ParserType  parser;

    const uint8_t *stream_base;
    uint32_t       stream_size;
    uint32_t       pos;             // byte offset into stream

    uint32_t    ticks_until_next;   // remaining ticks before NEXT event
    uint32_t    tempo_us_per_tick;  // current effective us/tick
    uint16_t    ppqn;
    uint32_t    accumulated_us;     // fractional carry between ticks

    uint8_t     running_status;     // for SMF running-status decoding

    bool        first_ad_event;     // PT_AD: first event has no leading VLQ
    bool        loop_song;          // PT_AD: restart on EOT (play_once=0)
    uint32_t    ad_track_start;     // PT_AD: byte offset of first event
    uint32_t    ad_track_end;       // PT_AD: byte offset just past last byte

    PendingEvent next;              // peeked but not yet dispatched

    uint32_t    total_ticks;        // monotonically advanced by imuse_tick;
                                    // exposed via imuse_get_music_timer().
};

static Song g_song{};

// ----------- helpers -----------

static uint32_t read_vlq(const uint8_t *p, uint32_t *off, uint32_t max) {
    uint32_t v = 0;
    while (*off < max) {
        uint8_t b = p[*off];
        (*off)++;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) return v;
    }
    return v;
}

static int find_smf_track_start(const uint8_t *data, uint32_t size, uint32_t *out_track_start, uint32_t *out_track_end, uint16_t *out_ppqn) {
    // MThd header: 4 'MThd' + 4 BE size (=6) + 2 BE format + 2 BE ntrks + 2 BE division
    if (size < 14) return -1;
    if (data[0]!='M'||data[1]!='T'||data[2]!='h'||data[3]!='d') return -1;
    uint32_t hdr_size = ((uint32_t)data[4]<<24)|((uint32_t)data[5]<<16)|((uint32_t)data[6]<<8)|data[7];
    uint16_t division = ((uint16_t)data[12]<<8)|data[13];
    if (division & 0x8000) return -1;          // SMPTE - we don't support
    *out_ppqn = division;
    uint32_t off = 8 + hdr_size;
    // Find first MTrk
    while (off + 8 <= size) {
        if (data[off]=='M'&&data[off+1]=='T'&&data[off+2]=='r'&&data[off+3]=='k') {
            uint32_t tsize = ((uint32_t)data[off+4]<<24)|((uint32_t)data[off+5]<<16)|((uint32_t)data[off+6]<<8)|data[off+7];
            *out_track_start = off + 8;
            *out_track_end = off + 8 + tsize;
            if (*out_track_end > size) *out_track_end = size;
            return 0;
        }
        off++;
    }
    return -1;
}

// Read a 4-byte little-endian uint32 at p+off (no bounds check).
static inline uint32_t rd_u32_le(const uint8_t *p, uint32_t off) {
    return  (uint32_t)p[off]
         | ((uint32_t)p[off+1] << 8)
         | ((uint32_t)p[off+2] << 16)
         | ((uint32_t)p[off+3] << 24);
}

// Walk the SO/SOUN payload and return the byte offset of the AD sub-chunk's
// PAYLOAD (i.e. just past its 6-byte header), and the AD payload length.
// Returns -1 if no AD chunk is found.
//
// SCUMM v4 SO body has the form:
//   <small_chunk:WA(...)> <small_chunk:AD(...)> [<small_chunk:SO(...)> ...]
// where each small_chunk = 4-byte LE size (inclusive of header) + 2-byte
// ASCII tag + body. Per scummvm sound.cpp:readSoundResourceSmallHeader, only
// the FIRST WA / AD pair is taken; nested SO chunks just bump the cursor.
static int find_ad_subchunk(Span data, uint32_t *out_payload_off,
                            uint32_t *out_payload_size) {
    uint32_t off = 0;
    // Don't drift past the resource. Each chunk is at least 6 bytes.
    while (off + 6 <= data.size) {
        uint32_t sz = rd_u32_le(data.data, off);
        if (sz < 6 || sz > data.size - off) return -1;
        uint8_t t0 = data.data[off + 4];
        uint8_t t1 = data.data[off + 5];
        if (t0 == 'A' && t1 == 'D') {
            *out_payload_off  = off + 6;
            *out_payload_size = sz - 6;
            return 0;
        }
        // Special case: nested SO. ScummVM advances by 6 (peek into it).
        if (t0 == 'S' && t1 == 'O') {
            off += 6;
            continue;
        }
        off += sz;
    }
    return -1;
}

// Decode the v5 floppy SOUN small-chunk payload to find the actual MIDI
// stream start.
//
// What we look at, in priority order:
//   - direct 'RO' header (Roland MT-32 sequence at offset 0)
//   - direct 'SO' or 'AD' / 'WA' tag at offsets 0/4 (small-chunk header)
//   - 'MThd' SMF header anywhere in first 64 bytes
//
// For the SO/WA/AD case (MI1 v4 floppy, what we mostly see in MI1 VGA
// floppy DOS), we always prefer the AD sub-chunk (AdLib), since the host
// audio is OPL2-only.
static int autodetect(Span data, ParserType *parser_out, uint32_t *body_off_out) {
    if (data.size < 6) return -1;
    const uint8_t *p = data.data;
    uint32_t lim = data.size < 64 ? (uint32_t)data.size : 64;

    // Direct RO header at offset 0 (Loom adds this to its tag-less Roland
    // resources). 'ROL' is something else (rolwave) so skip that.
    if (p[0]=='R'&&p[1]=='O' && (data.size < 3 || p[2]!='L')) {
        *parser_out = PT_RO;
        *body_off_out = 2;
        return 0;
    }

    // Small-chunk-style SO/WA/AD wrappers. The first 4 bytes are size; tag
    // is at offset 4-5. SO bodies hold nested WA/AD chunks; WA/AD at the
    // root means the resource was already partially unwrapped.
    bool tag_at_4 = (data.size >= 6);
    bool is_so = tag_at_4 && p[4]=='S' && p[5]=='O';
    bool is_wa = tag_at_4 && p[4]=='W' && p[5]=='A';
    bool is_ad = tag_at_4 && p[4]=='A' && p[5]=='D';

    if (is_so || is_wa) {
        // Walk the body looking for an AD sub-chunk. The body starts at +6
        // for SO; for WA the AD chunk is the SIBLING right after this WA
        // chunk (the SO body has been unwrapped already by the caller).
        uint32_t walk_off = is_so ? 6u : 0u;
        // Cap to body size if SO declares a size.
        uint32_t body_end = (uint32_t)data.size;
        if (is_so) {
            uint32_t so_total = rd_u32_le(p, 0);
            if (so_total >= 6 && so_total <= data.size) body_end = so_total;
        }
        Span body = data.sub(walk_off);
        body.size = body_end > walk_off ? body_end - walk_off : 0;
        uint32_t ad_off, ad_size;
        if (find_ad_subchunk(body, &ad_off, &ad_size) == 0) {
            *parser_out   = PT_AD;
            *body_off_out = walk_off + ad_off;
            return 0;
        }
        // No AD sub-chunk - we don't have a way to play WA/PC-speaker.
        return -1;
    }
    if (is_ad) {
        *parser_out = PT_AD;
        *body_off_out = 6;
        return 0;
    }

    // Search first 64 bytes for MThd / FORM (SMF / XMIDI).
    for (uint32_t off = 0; off + 4 < lim; off++) {
        if (p[off]=='M'&&p[off+1]=='T'&&p[off+2]=='h'&&p[off+3]=='d') {
            *parser_out = PT_SMF;
            *body_off_out = off;
            return 0;
        }
        // 'ADL ' = ADL music resource synthesized by scummvm; we don't see
        // those in stock data files - it's only built up internally.
        if (p[off]=='A'&&p[off+1]=='D'&&p[off+2]=='L'&&p[off+3]==' ') {
            // Body is at +8 (ADL  + size4) and contains MDhd/MThd; we
            // jump straight to MThd.
            for (uint32_t s = off + 8; s + 4 < (uint32_t)data.size && s < off + 64; s++) {
                if (p[s]=='M'&&p[s+1]=='T'&&p[s+2]=='h'&&p[s+3]=='d') {
                    *parser_out = PT_SMF;
                    *body_off_out = s;
                    return 0;
                }
            }
        }
    }
    return -1;
}

// ----------- public API -----------

void imuse_init() {
    memset(&g_song, 0, sizeof(g_song));
}

// Configure the song state for an 'AD' (AdLib) sub-chunk payload. The payload
// is `payload_size` bytes long starting at stream offset `body_off` (i.e.
// just past the small-chunk's 6-byte header). Returns false on truncation.
//
// Side effect: programs adlib_set_channel_instrument() for each instrument
// in the AD header. Caller has already cleared previous overrides.
static bool init_ad_song(Span sound_resource, uint32_t body_off,
                         uint32_t payload_size) {
    if (payload_size < 2 + 0x11 + 8 * 16) {
        platform::log("imuse: AD payload too small (%u bytes)\n", payload_size);
        return false;
    }
    const uint8_t *body = sound_resource.data + body_off;

    // Skip the 2-byte header stamp; layout follows convertADResource.
    const uint8_t *hdr = body + 2;
    uint8_t kind       = hdr[0];     // 0x80 = music, else SFX
    uint8_t ticks      = hdr[1];     // tempo input
    uint8_t play_once  = hdr[2];     // 1 = one-shot, 0 = loop (music only)
    uint8_t num_instr  = hdr[8];
    const uint8_t *channels = hdr + 9;
    const uint8_t *instr_table = hdr + 0x11;

    // We use convertADResource's tempo formula: dw = 500000 * 256 / ticks
    // microseconds per quarter note, with PPQN fixed at 480. Avoid div by
    // zero on bogus headers.
    constexpr uint32_t kAdPpqn = 480;
    if (ticks == 0) ticks = 128;
    uint32_t us_per_quarter = (uint32_t)((500000ull * 256ull) / (uint32_t)ticks);
    if (us_per_quarter < 1) us_per_quarter = kDefaultTempoUsPerQuarter;
    uint32_t us_per_tick = us_per_quarter / kAdPpqn;
    if (us_per_tick == 0) us_per_tick = 1;

    // Bind per-channel custom instruments. Each instrument record is 14
    // bytes; bytes [3..12] are the OPL2 mod/car FM definition and byte
    // [2] is feedback (matches scummvm convertADResource layout). We pack
    // those 11 bytes in the order the AdLib driver expects.
    if (num_instr > 8) num_instr = 8;
    for (uint8_t i = 0; i < num_instr; i++) {
        uint8_t midi_ch_1based = channels[i];
        if (midi_ch_1based == 0 || midi_ch_1based > 16) continue;
        const uint8_t *src = instr_table + (size_t)i * 16;
        uint8_t def[11];
        def[0]  = src[3];   // mod_characteristic
        def[1]  = src[4];   // mod_scalingOutputLevel
        def[2]  = src[5];   // mod_attackDecay
        def[3]  = src[6];   // mod_sustainRelease
        def[4]  = src[7];   // mod_waveformSelect
        def[5]  = src[8];   // car_characteristic
        def[6]  = src[9];   // car_scalingOutputLevel
        def[7]  = src[10];  // car_attackDecay
        def[8]  = src[11];  // car_sustainRelease
        def[9]  = src[12];  // car_waveformSelect
        def[10] = src[2];   // feedback
        adlib_set_channel_instrument((uint8_t)(midi_ch_1based - 1), def);
    }

    // Track payload starts after 2 + 0x11 + 8 * 16 = 147 bytes.
    constexpr uint32_t kAdPrefixBytes = 2 + 0x11 + 8 * 16;
    g_song.ad_track_start = body_off + kAdPrefixBytes;
    g_song.ad_track_end   = body_off + payload_size;
    g_song.pos            = g_song.ad_track_start;
    g_song.stream_size    = g_song.ad_track_end;
    g_song.first_ad_event = true;
    g_song.loop_song      = (kind == 0x80) && (play_once == 0);
    g_song.ppqn           = kAdPpqn;
    g_song.tempo_us_per_tick = us_per_tick;
    g_song.running_status = 0;

    platform::log("imuse: AD sound: kind=%s ticks=%u play_once=%u num_instr=%u "
                  "tempo=%u us/quarter (%u us/tick)\n",
                  kind == 0x80 ? "MUSIC" : "SFX",
                  (unsigned)ticks, (unsigned)play_once, (unsigned)num_instr,
                  us_per_quarter, us_per_tick);
    return true;
}

bool imuse_start_sound(int sound_id, Span sound_resource) {
    if (sound_resource.empty()) {
        platform::log("imuse: sound %d has empty resource\n", sound_id);
        return false;
    }
    ParserType pt;
    uint32_t body_off;
    if (autodetect(sound_resource, &pt, &body_off) < 0) {
        // Print first bytes for debugging
        const uint8_t *p = sound_resource.data;
        size_t n = sound_resource.size < 16 ? sound_resource.size : 16;
        char hex[64]; int hn = 0;
        for (size_t i = 0; i < n; i++) hn += snprintf(hex+hn, sizeof(hex)-hn, "%02X ", p[i]);
        platform::log("imuse: sound %d unknown format (size=%zu, hdr: %s)\n",
                      sound_id, sound_resource.size, hex);
        return false;
    }

    // Stop existing
    if (g_song.playing) {
        adlib_silence_all();
    }
    // Always clear leftover per-channel custom instruments from a prior AD
    // song; PT_RO/PT_SMF rely on GM lookup.
    adlib_clear_channel_instruments();

    g_song.playing = true;
    g_song.sound_id = sound_id;
    g_song.parser = pt;
    g_song.stream_base = sound_resource.data;
    g_song.stream_size = (uint32_t)sound_resource.size;
    g_song.pos = body_off;
    g_song.ticks_until_next = 0;
    g_song.accumulated_us = 0;
    g_song.running_status = 0;
    g_song.next.valid = false;
    g_song.first_ad_event = false;
    g_song.loop_song = false;
    g_song.ad_track_start = 0;
    g_song.ad_track_end = 0;
    g_song.total_ticks = 0;

    if (pt == PT_RO) {
        g_song.ppqn = kRoPpqn;
        g_song.tempo_us_per_tick = kRoTempoUsPerTick;
    } else if (pt == PT_AD) {
        // Compute AD payload size. The autodetect points us at the START
        // of the AD payload; we need the size from the small-chunk header
        // 6 bytes earlier.
        uint32_t ad_chunk_off = body_off - 6;
        uint32_t ad_chunk_sz  = rd_u32_le(sound_resource.data, ad_chunk_off);
        if (ad_chunk_sz < 6 || ad_chunk_sz > (uint32_t)sound_resource.size - ad_chunk_off) {
            platform::log("imuse: AD chunk header truncated (sound %d)\n", sound_id);
            g_song.playing = false;
            return false;
        }
        uint32_t payload_size = ad_chunk_sz - 6;
        if (!init_ad_song(sound_resource, body_off, payload_size)) {
            g_song.playing = false;
            return false;
        }
    } else {
        // SMF: parse header for ppqn, find first track.
        uint32_t track_start = 0, track_end = g_song.stream_size;
        uint16_t ppqn = 480;
        if (find_smf_track_start(sound_resource.data, (uint32_t)sound_resource.size,
                                 &track_start, &track_end, &ppqn) == 0) {
            g_song.pos = track_start;
            g_song.stream_size = track_end;     // limit to first track
        }
        g_song.ppqn = ppqn ? ppqn : 480;
        g_song.tempo_us_per_tick = kDefaultTempoUsPerQuarter / g_song.ppqn;
    }

    const char *pname = pt == PT_RO  ? "RO/SO"
                      : pt == PT_AD  ? "AD"
                      : pt == PT_SMF ? "SMF"
                                     : "?";
    platform::log("imuse: starting sound %d (parser=%s body_off=%u size=%u)\n",
                  sound_id, pname, body_off, (unsigned)sound_resource.size);

    return true;
}

void imuse_stop_sound(int sound_id) {
    if (g_song.playing && g_song.sound_id == sound_id) {
        g_song.playing = false;
        g_song.next.valid = false;
        adlib_silence_all();
    }
}

void imuse_stop_all() {
    g_song.playing = false;
    g_song.next.valid = false;
    adlib_silence_all();
}

bool imuse_is_running(int sound_id) {
    return g_song.playing && g_song.sound_id == sound_id;
}

int imuse_current_sound() {
    return g_song.playing ? g_song.sound_id : 0;
}

// ---------------- peek next event into g_song.next ----------------
//
// On entry: g_song.pos points to the first byte of the next event in the
// stream. On success: fills g_song.next with delta + event, advances pos
// past the event. On stream end: clears g_song.next.valid.

static void peek_next_ro_event() {
    g_song.next.valid = false;
    g_song.next.meta_type = 0;
    uint32_t delta = 0;
    while (g_song.pos < g_song.stream_size) {
        uint8_t status = g_song.stream_base[g_song.pos++];
        if (status == 0xF0) {
            if (g_song.pos >= g_song.stream_size) return;
            delta += g_song.stream_base[g_song.pos++];
            continue;
        }
        if (status == 0xF1) {
            if (g_song.pos >= g_song.stream_size) return;
            delta += 256 + g_song.stream_base[g_song.pos++];
            continue;
        }
        if (status == 0xFF || status == 0x00) {
            // EOT (FF = autoloop, 00 = explicit end).
            return;
        }
        uint8_t cmd = status & 0xF0;
        if (cmd == 0xA0) {
            if (g_song.pos + 1 >= g_song.stream_size) return;
            g_song.pos += 2;
            continue;
        }
        if (status < 0x80) {
            // unexpected byte - skip
            continue;
        }
        if (cmd == 0xC0 || cmd == 0xD0) {
            if (g_song.pos >= g_song.stream_size) return;
            g_song.next.status = status;
            g_song.next.d1 = g_song.stream_base[g_song.pos++];
            g_song.next.d2 = 0;
        } else {
            if (g_song.pos + 1 >= g_song.stream_size) return;
            g_song.next.status = status;
            g_song.next.d1 = g_song.stream_base[g_song.pos++];
            g_song.next.d2 = g_song.stream_base[g_song.pos++];
        }
        g_song.next.delta_ticks = delta;
        g_song.next.valid = true;
        return;
    }
}

static void peek_next_smf_event() {
    g_song.next.valid = false;
    g_song.next.meta_type = 0;
    while (g_song.pos < g_song.stream_size) {
        uint32_t delta = read_vlq(g_song.stream_base, &g_song.pos, g_song.stream_size);
        if (g_song.pos >= g_song.stream_size) return;

        uint8_t status = g_song.stream_base[g_song.pos];
        if (status & 0x80) {
            g_song.pos++;
            g_song.running_status = status;
        } else {
            status = g_song.running_status;
        }

        if (status == 0xFF) {
            if (g_song.pos >= g_song.stream_size) return;
            uint8_t mtype = g_song.stream_base[g_song.pos++];
            uint32_t mlen = read_vlq(g_song.stream_base, &g_song.pos, g_song.stream_size);
            if (g_song.pos + mlen > g_song.stream_size) return;
            const uint8_t *mdata = g_song.stream_base + g_song.pos;
            g_song.pos += mlen;
            if (mtype == 0x2F) return;        // EOT
            if (mtype == 0x51 && mlen == 3) {
                uint32_t tempo = ((uint32_t)mdata[0]<<16)|((uint32_t)mdata[1]<<8)|mdata[2];
                if (tempo == 0) tempo = kDefaultTempoUsPerQuarter;
                g_song.next.delta_ticks = delta;
                g_song.next.status = 0xFF;
                g_song.next.meta_type = 0x51;
                g_song.next.meta_tempo_us_per_quarter = tempo;
                g_song.next.valid = true;
                return;
            }
            // Other meta - emit no-op to consume the delta.
            g_song.next.delta_ticks = delta;
            g_song.next.status = 0;
            g_song.next.valid = true;
            return;
        }
        if (status == 0xF0 || status == 0xF7) {
            uint32_t mlen = read_vlq(g_song.stream_base, &g_song.pos, g_song.stream_size);
            if (g_song.pos + mlen > g_song.stream_size) return;
            g_song.pos += mlen;
            g_song.next.delta_ticks = delta;
            g_song.next.status = 0;
            g_song.next.valid = true;
            return;
        }

        uint8_t cmd = status & 0xF0;
        if (cmd == 0xC0 || cmd == 0xD0) {
            if (g_song.pos >= g_song.stream_size) return;
            g_song.next.status = status;
            g_song.next.d1 = g_song.stream_base[g_song.pos++];
            g_song.next.d2 = 0;
        } else if (cmd >= 0x80 && cmd <= 0xE0) {
            if (g_song.pos + 1 >= g_song.stream_size) return;
            g_song.next.status = status;
            g_song.next.d1 = g_song.stream_base[g_song.pos++];
            g_song.next.d2 = g_song.stream_base[g_song.pos++];
        } else {
            continue;
        }
        g_song.next.delta_ticks = delta;
        g_song.next.valid = true;
        return;
    }
}

// AD parser: same shape as SMF, but the FIRST event in the track has no
// leading VLQ delta (it's effectively delta=0 for a music start; any
// pre-roll comes from convertADResource's `ppqn/3` synthesized prefix and
// we ignore that to keep the implementation simple - it's ~100ms).
//
// Subsequent events: <VLQ delta> <status-or-running-status> <params>.
// EOT is detected by reading past the AD track end. SysEx (0xF0/0xF7) and
// meta events (0xFF) are NOT expected in the raw AD track payload, but if
// they show up we skip them (the AD track is a synthesized fragment of
// what becomes an SMF in scummvm's pipeline; meta events were prepended
// by the converter, not present in the raw bytes).
static void peek_next_ad_event() {
    g_song.next.valid = false;
    g_song.next.meta_type = 0;

    while (g_song.pos < g_song.stream_size) {
        uint32_t delta;
        if (g_song.first_ad_event) {
            delta = 0;
            g_song.first_ad_event = false;
        } else {
            delta = read_vlq(g_song.stream_base, &g_song.pos, g_song.stream_size);
            if (g_song.pos >= g_song.stream_size) return;
        }

        uint8_t status = g_song.stream_base[g_song.pos];
        if (status & 0x80) {
            g_song.pos++;
            g_song.running_status = status;
        } else {
            status = g_song.running_status;
            if (status == 0) {
                // No prior status to fall back on; bail.
                return;
            }
        }

        // SysEx / meta in the raw track shouldn't really happen (they get
        // emitted by convertADResource's wrapping), but be defensive.
        if (status == 0xF0 || status == 0xF7) {
            // SysEx without leading length byte in raw form - just walk
            // until 0xF7 to keep parser alive.
            while (g_song.pos < g_song.stream_size &&
                   g_song.stream_base[g_song.pos] != 0xF7) {
                g_song.pos++;
            }
            if (g_song.pos < g_song.stream_size) g_song.pos++;
            g_song.next.delta_ticks = delta;
            g_song.next.status = 0;     // no-op
            g_song.next.valid = true;
            return;
        }
        if (status == 0xFF) {
            // Treat as EOT
            return;
        }

        uint8_t cmd = status & 0xF0;
        if (cmd == 0xC0 || cmd == 0xD0) {
            if (g_song.pos >= g_song.stream_size) return;
            g_song.next.status = status;
            g_song.next.d1 = g_song.stream_base[g_song.pos++];
            g_song.next.d2 = 0;
        } else if (cmd >= 0x80 && cmd <= 0xE0) {
            if (g_song.pos + 1 >= g_song.stream_size) return;
            g_song.next.status = status;
            g_song.next.d1 = g_song.stream_base[g_song.pos++];
            g_song.next.d2 = g_song.stream_base[g_song.pos++];
        } else {
            // Garbage byte - skip and try again.
            continue;
        }
        g_song.next.delta_ticks = delta;
        g_song.next.valid = true;
        return;
    }
}

static void peek_next() {
    switch (g_song.parser) {
        case PT_RO:  peek_next_ro_event();  return;
        case PT_AD:  peek_next_ad_event();  return;
        case PT_SMF: peek_next_smf_event(); return;
        default:     g_song.next.valid = false; return;
    }
}

static void dispatch_pending() {
    if (!g_song.next.valid) return;
    uint8_t status = g_song.next.status;
    if (status == 0xFF && g_song.next.meta_type == 0x51) {
        uint16_t ppqn = g_song.ppqn ? g_song.ppqn : 480;
        uint32_t v = g_song.next.meta_tempo_us_per_quarter / ppqn;
        if (v == 0) v = 1;
        g_song.tempo_us_per_tick = v;
    } else if (status >= 0x80 && status <= 0xEF) {
        adlib_midi_event(status, g_song.next.d1, g_song.next.d2);
    }
    // status == 0 is no-op
    g_song.next.valid = false;
}

// Try to restart the current song from the beginning if it's a looping
// PT_AD song. Returns true if we successfully looped and primed the next
// event; false otherwise.
static bool try_loop_ad_song() {
    if (g_song.parser != PT_AD || !g_song.loop_song) return false;
    g_song.pos = g_song.ad_track_start;
    g_song.first_ad_event = true;
    g_song.running_status = 0;
    g_song.accumulated_us = 0;
    peek_next();
    if (!g_song.next.valid) return false;
    g_song.ticks_until_next = g_song.next.delta_ticks;
    return true;
}

void imuse_tick(uint32_t elapsed_us) {
    if (!g_song.playing) return;
    if (g_song.tempo_us_per_tick == 0) g_song.tempo_us_per_tick = 1;

    g_song.accumulated_us += elapsed_us;
    uint32_t ticks_elapsed = g_song.accumulated_us / g_song.tempo_us_per_tick;
    g_song.accumulated_us -= ticks_elapsed * g_song.tempo_us_per_tick;
    g_song.total_ticks += ticks_elapsed;

    // Lazy-prime: if we don't have a pending event, peek the first one.
    if (!g_song.next.valid) {
        peek_next();
        if (!g_song.next.valid && !try_loop_ad_song()) {
            // Stream is over.
            g_song.playing = false;
            adlib_silence_all();
            platform::log("imuse: sound %d ended\n", g_song.sound_id);
            return;
        }
        g_song.ticks_until_next = g_song.next.delta_ticks;
    }

    int safety = 4096;
    while (g_song.playing && ticks_elapsed >= g_song.ticks_until_next && --safety > 0) {
        ticks_elapsed -= g_song.ticks_until_next;
        // Dispatch the pending event.
        dispatch_pending();
        // Peek the next.
        peek_next();
        if (!g_song.next.valid) {
            if (try_loop_ad_song()) {
                // ticks_until_next set by try_loop_ad_song().
                continue;
            }
            g_song.playing = false;
            adlib_silence_all();
            platform::log("imuse: sound %d ended\n", g_song.sound_id);
            return;
        }
        g_song.ticks_until_next = g_song.next.delta_ticks;
    }
    if (g_song.playing) {
        g_song.ticks_until_next -= ticks_elapsed;
    }
}

// Mirrors ScummVM Player::getMusicTimer (imuse_player.cpp:133):
//   ticks * 2 / PPQN
// VAR_MUSIC_TIMER then equals timer * timer_freq / 240. Default freq is 240,
// so the var ends up as ticks*2/PPQN (integer beats * 2).
//
// We allow a debug speedup multiplier (TSB_MUSIC_TIMER_SCALE env var) so
// trace runs can advance past long music-wait loops within a short test
// budget. Real playback still uses the unmodified parser tick.
int imuse_get_music_timer() {
    if (!g_song.playing || g_song.ppqn == 0) return 0;
    // Real-time music timer on host AND device. The earlier 10x host fudge
    // traded trace correctness for fast smoke runs — but it caused
    // music-driven script counters (e.g. Script 149's stage variable at
    // global slot 100) to advance ahead of upstream, so our trace and
    // ScummVM's diverged once the intro entered any music-gated branch.
    return (int)((g_song.total_ticks * 2u) / g_song.ppqn);
}

}  // namespace tsb
