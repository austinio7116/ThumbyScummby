// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// Text / string pipeline. Mirrors the v4 + v5 paths in
// scummvm-upstream/engines/scumm/{string,charset,actor,script_v5}.cpp:
//
//   o5_print()                                    (script_v5.cpp:2058)
//     -> _actorToPrintStrFor; decodeParseString();
//   decodeParseString() {                         (script_v5.cpp:3443)
//     textSlot from actorToPrint (252/253/254/else -> 3/2/1/0);
//     loadDefault(); SO_xx loop; SO_TEXTSTRING -> printString(slot,ptr);
//   }
//   printString(0, msg) -> actorTalk(msg);        (string.cpp:107)
//   printString(1, msg) -> drawString(1, msg);    (string.cpp:113)
//   actorTalk:                                    (actor.cpp:3466)
//     convertMessageToString -> _charsetBuffer;
//     setTalkingActor; _haveMsg=0xFF; VAR_HAVE_MSG=0xFF; _talkDelay=0;
//     displayDialog().
//   displayDialog (per-frame):                    (string.cpp:1033)
//     while talkDelay > 0 -> return.
//     handleNextCharsetCode + charset->printChar.
//
// drawString (the verb / sentence path):          (string.cpp:1354)
//     convertMessageToString -> buf; setColor; for each c: printChar.

#include "text.h"
#include "vm.h"
#include "platform.h"
#include "charset.h"
#include "actor.h"
#include "engine.h"

#include <string.h>

namespace tsb {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static StringSettings g_string[NUM_STRING_SLOTS];
static StringSettings g_string_default[NUM_STRING_SLOTS];

// ScummEngine _charsetBuffer / _charsetBufPos / _haveMsg / _talkDelay /
// _haveActorSpeechMsg / _useTalkAnims / _charsetColor / _talkingActor /
// _keepText / _nextLeft / _nextTop. We hold flat copies here.
static uint8_t  g_charset_buffer[1024];
static int      g_charset_buf_len = 0;
static int      g_charset_buf_pos = 0;
static uint8_t  g_have_msg = 0;
static int      g_talk_delay = 0;
static uint8_t  g_charset_color = 15;
static int      g_talking_actor = 0xFF;
static bool     g_keep_text = false;
static int      g_next_left = 0;
static int      g_next_top  = 0;
static int      g_actor_to_print_for = 0xFF;

// Loaded charset (1 = main game font for MI1, comes from 901.LFL).
// ScummVM has a per-charset _charsetData[16] colour map (used for >=2bpp
// glyphs). v4 uses charset id 0 (in MI1) or 1 (helpers). For now we
// load a single charset and apply a flat 1bpp color path.
static Charset  g_charsets[8];
static bool     g_charset_loaded[8];
static int      g_cur_charset_id = 0;
static uint8_t  g_charset_color_map[16];   // _charsetData[curId][0..15]

// _actorToPrintStrFor (script_v5.cpp:2074, set by o5_print).

void string_init() {
    memset(g_string, 0, sizeof(g_string));
    memset(g_string_default, 0, sizeof(g_string_default));
    memset(g_charsets, 0, sizeof(g_charsets));
    memset(g_charset_loaded, 0, sizeof(g_charset_loaded));
    // _charsetColorMap defaults to all zeros (matches
    // ScummEngine::ScummEngine constructor at scumm.cpp:240
    // `memset(_charsetColorMap, 0, sizeof(_charsetColorMap))`).
    // Index 1 gets the live talk colour at draw time
    // (charset.cpp:1100 `_vm->_charsetColorMap[1] = _color`); other
    // entries stay 0 = palette index 0 = transparent/black, which
    // for v4 2bpp glyphs renders as the dark "shadow" pixels under
    // the foreground colour.
    memset(g_charset_color_map, 0, sizeof(g_charset_color_map));
}

void string_load_default(int slot) {
    if (slot < 0 || slot >= NUM_STRING_SLOTS) return;
    g_string[slot] = g_string_default[slot];
}

void string_save_default(int slot) {
    if (slot < 0 || slot >= NUM_STRING_SLOTS) return;
    g_string_default[slot] = g_string[slot];
}

void string_set_default_charset(int slot, int charset_id) {
    if (slot < 0 || slot >= NUM_STRING_SLOTS) return;
    if (charset_id < 0 || charset_id > 0xFF) return;
    g_string_default[slot].charset = (uint8_t)charset_id;
    // Also push to the live slot so an in-progress print picks the
    // change up immediately. ScummVM's initCharset only sets the
    // default, but talk text reads via load_default each call so it
    // converges to the same effective value within one frame.
    g_string[slot].charset = (uint8_t)charset_id;
}

StringSettings *string_get(int slot) {
    if (slot < 0 || slot >= NUM_STRING_SLOTS) return nullptr;
    return &g_string[slot];
}

bool string_have_message()      { return g_have_msg != 0; }
int  string_talk_delay()        { return g_talk_delay; }
int  string_get_talking_actor() { return g_talking_actor; }
void string_set_keep_text(bool b) { g_keep_text = b; }

void string_set_charset_colormap(const uint8_t *table, int n) {
    if (!table) return;
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) g_charset_color_map[i] = table[i];
}

// ---------------------------------------------------------------------------
// Charset loading
// ---------------------------------------------------------------------------

// Load a charset from 9xx.LFL. ScummVM o5_resourceRoutines case 18 (LOAD_CHARSET).
// Charset 0 = main font; helper IDs 1..3 = secondary fonts.
static bool ensure_charset(int id) {
    if (id < 0 || id >= 8) return false;
    if (g_charset_loaded[id]) return true;
    // Mirrors ScummEngine_v4::loadCharset (resource_v4.cpp:178-198):
    //   sprintf_s(buf, "%03d.LFL", 900 + no);
    // So charset id N maps to file 9NN.LFL — id 0 is invalid (no
    // 900.LFL ships with MI1), id 1 = 901.LFL, id 2 = 902.LFL, etc.
    // (A previous reading of this used `901 + id` which made
    // initCharset(N) load the wrong helper file.)
    int helper = 900 + id;
    if (charset_load_from_helper(helper, &g_charsets[id])) {
        g_charset_loaded[id] = true;
        return true;
    }
    return false;
}

// Mirrors ScummVM CharsetRendererCommon::setCurID + memcpy(_charsetColorMap,
// _charsetData[id], 4) (string.cpp:1426 / 1104). The 4-bit colour map
// that talk-text uses is supplied by o5_cursorCommand sub 14 (the
// vararg word list in opcodes.cpp); when no script has called it the
// colour map left at default identity (0..15) which matches ScummVM's
// startup state before the boot script's first cursorCommand.
static void set_cur_charset(int id) {
    if (id < 0 || id >= 8) return;
    if (!ensure_charset(id)) return;
    g_cur_charset_id = id;
}

// ---------------------------------------------------------------------------
// Glue: where do we draw? main viewport buffer, accessed via engine.
// ---------------------------------------------------------------------------

extern uint8_t *engine_main_vscreen();          // host bridge — engine.cpp
extern int      engine_main_vscreen_pitch();    // == VIRTUAL_SCREEN_W

// ---------------------------------------------------------------------------
// convertMessageToString — \xFF escape resolution
// ---------------------------------------------------------------------------
//
// Mirrors ScummEngine::convertMessageToString (string.cpp:1573-1742). For
// each \xFF cN (control N), N==1/2/3/8 are simple "keep" markers (passed
// through as 0xFF cN to the per-frame loop); 4..10 are substitutions
// taking 2 operand bytes (a 16-bit var ID): we expand them inline.
//   4 = integer var: convert to ASCII decimal
//   5 = verb-name resource (we don't model verbs yet)
//   6 = obj-or-actor name (no name table loaded — return empty)
//   7 = nested string slot (no string-slot pool yet — return empty)
//   9, 0xA = audio start markers — passed through (they affect timing,
//            not text), four extra bytes total per ScummVM encoding.
//
// The resulting buffer is consumed by displayDialog/drawString.

static int append_int(uint8_t *dst, int max, int val) {
    char buf[16];
    int n = 0;
    if (val < 0) { if (n < max) dst[n++] = '-'; val = -val; }
    char tmp[12];
    int t = 0;
    if (val == 0) tmp[t++] = '0';
    while (val > 0 && t < 11) { tmp[t++] = (char)('0' + (val % 10)); val /= 10; }
    while (t-- > 0 && n < max) buf[n++] = tmp[t];
    int copy = n; if (copy > max) copy = max;
    for (int i = 0; i < copy; i++) dst[i] = (uint8_t)buf[i];
    return copy;
}

int string_convert_message(const uint8_t *src, uint8_t *dst, int dst_size) {
    if (!src || !dst || dst_size <= 0) return 0;
    uint8_t *out = dst;
    uint8_t *end = dst + dst_size - 1;     // leave space for terminator
    while (out < end) {
        uint8_t c = *src++;
        if (c == 0) break;
        if (c == 0xFF) {
            uint8_t code = *src++;
            uint16_t val = (uint16_t)src[0] | ((uint16_t)src[1] << 8);
            switch (code) {
            case 1: case 2: case 3: case 8:
                // Pass-through escape (newline / keep / wait / keep2) —
                // displayDialog reads them via handleNextCharsetCode.
                if (out + 2 <= end) { *out++ = 0xFF; *out++ = code; }
                break;
            case 4: { // substitute integer var (var id is the 16-bit val)
                int n = (int)g_vm.globals[val < VM_NUM_GLOBALS ? val : 0];
                int wrote = append_int(out, (int)(end - out), n);
                out += wrote;
                src += 2;
                break;
            }
            case 5: { // substitute verb name — no verb names loaded
                src += 2;
                break;
            }
            case 6: { // substitute object/actor name — none loaded
                src += 2;
                break;
            }
            case 7: { // substitute string slot — none loaded
                src += 2;
                break;
            }
            case 9: case 0x0A: case 0x0C: case 0x0D: case 0x0E:
                // Audio / sound / animation markers. ScummVM passes them
                // through verbatim into _charsetBuffer with a 4-byte
                // payload (string.cpp:1666-1671). displayDialog interprets
                // them via handleNextCharsetCode.
                if (out + 4 <= end) {
                    *out++ = 0xFF; *out++ = code;
                    *out++ = src[0]; *out++ = src[1];
                }
                src += 2;
                break;
            default:
                // Unknown escape — drop and rewind 2 bytes per ScummVM
                // (string.cpp:1690 "num -= 2"), so we don't consume the
                // operand and treat the codes as raw chars.
                src -= 2;
                break;
            }
        } else if (c == '@') {
            // ScummVM strips '@' (filler char) — string.cpp:1696.
            continue;
        } else {
            *out++ = c;
        }
    }
    *out = 0;
    return (int)(out - dst);
}

// ---------------------------------------------------------------------------
// Rendering — flat draw of a converted buffer at (xpos, ypos, color)
// ---------------------------------------------------------------------------

static int charset_glyph_advance(const Charset *cs, char c) {
    int idx = (uint8_t)c;
    if (idx >= cs->glyph_count) return 4;
    uint32_t offs = read_le32(cs->glyph_offsets + idx * 4);
    if (offs == 0) return 4;
    size_t fontptr_base = (size_t)(cs->fontptr - cs->resource.data);
    if (fontptr_base + offs >= cs->resource.size) return 4;
    return cs->fontptr[offs];
}

static int draw_string_at(int x, int y, uint8_t color,
                          const uint8_t *buf, bool center) {
    if (!g_charset_loaded[g_cur_charset_id]) return x;
    Charset *cs = &g_charsets[g_cur_charset_id];

    uint8_t *vscr = engine_main_vscreen();
    int pitch = engine_main_vscreen_pitch();
    if (!vscr) return x;

    // Build a per-call colour table: index 0 transparent, index 1 ==
    // color (1bpp glyphs use bit value 1 for foreground). Higher bpp
    // glyphs would need _charsetColorMap; keep that for later.
    uint8_t color_table[16];
    color_table[0] = 0;     // transparent — charset_draw_char skips zero
    color_table[1] = color;
    for (int i = 2; i < 16; i++) color_table[i] = g_charset_color_map[i];

    // Compute string width if centered.
    int total_w = 0;
    if (center) {
        for (int i = 0; buf[i] != 0; i++) {
            uint8_t c = buf[i];
            if (c == 0xFF) {
                uint8_t code = buf[i+1];
                if (code == 1 || code == 2 || code == 3 || code == 8) {
                    i += 1; continue;
                }
                i += 3;
                continue;
            }
            total_w += charset_glyph_advance(cs, (char)c);
        }
        x -= total_w / 2;
    }

    int cx = x, cy = y;
    for (int i = 0; buf[i] != 0; i++) {
        uint8_t c = buf[i];
        if (c == 0xFF) {
            uint8_t code = buf[i+1];
            if (code == 1) {     // newline
                cx = x; cy += cs->glyph_height + 1;
                i += 1; continue;
            }
            if (code == 2 || code == 3 || code == 8) { i += 1; continue; }
            // skip 4-byte escape payload
            i += 3;
            continue;
        }
        if (c == '\n') {
            cx = x; cy += cs->glyph_height + 1;
            continue;
        }
        charset_draw_char(cs, (char)c, cx, cy, color_table, vscr, pitch);
        cx += charset_glyph_advance(cs, (char)c);
    }
    return cx;
}

// ---------------------------------------------------------------------------
// drawString — the verb / sentence-line path
// ---------------------------------------------------------------------------
//
// Mirrors ScummEngine::drawString (string.cpp:1354+). Sets up the
// charset, converts the message, then draws character-by-character.
static void draw_string(int slot, const uint8_t *msg) {
    if (slot < 0 || slot >= NUM_STRING_SLOTS) return;
    StringSettings *s = &g_string[slot];

    uint8_t buf[270];
    string_convert_message(msg, buf, sizeof(buf));

    set_cur_charset(s->charset);
    if (!g_charset_loaded[g_cur_charset_id]) return;

    g_charset_color = s->color;
    g_next_left = s->xpos;
    g_next_top  = s->ypos;
    draw_string_at(s->xpos, s->ypos, s->color, buf, s->center);

    // ScummVM advances _string[a].xpos to the right edge of the
    // rendered glyph run so successive printString calls on the same
    // slot continue from where the last one stopped (string.cpp:120).
    // Our draw_string_at writes left-to-right starting at s->xpos and
    // the dispatcher already publishes the right edge via g_next_left;
    // leaving s->xpos at its caller-supplied value matches v4 boot
    // scripts which always re-set xpos on every print, but breaks
    // multi-call concatenation. Sync the slot's xpos to g_next_left so
    // the next call lands at the correct position.
    s->xpos = (int16_t)g_next_left;
}

// ---------------------------------------------------------------------------
// printString — the slot dispatcher (mirrors string.cpp:57-124)
// ---------------------------------------------------------------------------
void string_print(int slot, const uint8_t *msg) {
    if (slot == 0) {
        string_actor_talk(g_actor_to_print_for, msg);
    } else if (slot == 1 || slot == 2 || slot == 3) {
        // Verb name / debug / message dialog — ScummVM routes through
        // drawString(1) / debugMessage / showMessageDialog. We treat
        // 1/2/3 as "draw immediately" so the verb bar can render.
        draw_string(slot, msg);
    }
}

// ---------------------------------------------------------------------------
// actorTalk + displayDialog tick
// ---------------------------------------------------------------------------
//
// Mirrors ScummEngine::actorTalk (actor.cpp:3466) — for v4/v5 the
// procedure is: convertMessageToString -> _charsetBuffer; if !_keepText
// stopTalk; setTalkingActor; pick _charsetColor from actor's talkColor;
// _haveMsg = 0xFF; VAR_HAVE_MSG = 0xFF; _talkDelay = 0; displayDialog.
void string_actor_talk(int actor_to_print_for, const uint8_t *msg) {
    // Order matters: ScummEngine::actorTalk (actor.cpp:3466-3520) calls
    // stopTalk() BEFORE convertMessageToString. We had it backwards —
    // converted into g_charset_buffer first, then stop_talk reset
    // buf_len/buf_pos to 0, then set g_have_msg. Result: string_tick
    // saw g_have_msg=0xFF but buf_len=0 so never emitted any glyphs.
    if (actor_to_print_for == 0xFF) {
        if (!g_keep_text) string_stop_talk();
        g_talking_actor = 0xFF;
        // No actor → take the colour from the per-call slot 0 setting,
        // which decodeParseString just populated via SO_COLOR. (Mirrors
        // ScummEngine::actorTalk colour-of-narrator path: when there is
        // no _actorToPrintStrFor, _charsetColor stays in sync with
        // _string[0].color.) Without this we'd render the splash text
        // (Lucasfilm credits, "Deep in the Caribbean…") in whatever
        // colour the previous talk left in _charsetColor.
        g_charset_color = g_string[0].color;
    } else {
        Actor *a = actor_get(actor_to_print_for);
        if (!a) return;
        if (!g_keep_text) string_stop_talk();
        g_talking_actor = actor_to_print_for;
        // runActorTalkScript(_talkStartFrame) — actor.cpp:3505. With no
        // talk-script, this is just startAnimActor(_talkStartFrame).
        a->frame = a->talk_start_frame;
        g_charset_color = a->talk_color;
    }

    int n = string_convert_message(msg, g_charset_buffer,
                                   sizeof(g_charset_buffer));
    g_charset_buf_len = n;
    g_charset_buf_pos = 0;

    g_have_msg = 0xFF;
    g_vm.globals[VAR_HAVE_MSG] = 0xFF;
    g_talk_delay = 0;
}

void string_stop_talk() {
    g_have_msg = 0;
    g_talk_delay = 0;
    if (g_talking_actor != 0 && g_talking_actor < 0x80) {
        Actor *a = actor_get(g_talking_actor);
        if (a) a->frame = a->talk_stop_frame;
    }
    // ScummVM v3-7 path: setTalkingActor(0xFF) — preserves history.
    g_talking_actor = 0xFF;
    g_vm.globals[VAR_HAVE_MSG] = 0;
    g_charset_buf_pos = 0;
    g_charset_buf_len = 0;
    // Mirrors actor.cpp:3598 — stopTalk also clears _keepText so the
    // sticky "keep prior text on screen" flag from a \xFF\x02 escape
    // does not survive past the next stopTalk. Without this, every
    // print after one \xFF\x02 would stack on top of the previous,
    // producing the credit-roll overlap behaviour.
    g_keep_text = false;
    // Clear the text overlay so the previous talk's pixels don't bleed
    // into the next room. Mirrors ScummVM restoreCharsetBg which is
    // called on stopTalk + scene change. Audit H88.
    engine_clear_text_vscreen();
}

// Mirrors ScummEngine::displayDialog (string.cpp:1033-1331), the
// per-frame heart of talk. Decrements _talkDelay and emits the next
// glyph(s) when the delay reaches zero. Stops when the message ends.
void string_tick() {
    if (!g_have_msg) return;

    // Decrement talk delay by VAR_TIMER value (ScummVM also subtracts
    // _talkDelay in scumm.cpp:3215). When the delay drops to <=0 we
    // emit the next character.
    if (g_talk_delay > 0) {
        int dt = (int)g_vm.globals[VAR_TIMER];
        if (dt <= 0) dt = 4;
        g_talk_delay -= dt;
        if (g_talk_delay > 0) return;
    }

    set_cur_charset(g_string[0].charset);
    if (!g_charset_loaded[g_cur_charset_id]) return;
    Charset *cs = &g_charsets[g_cur_charset_id];

    uint8_t *vscr = engine_main_vscreen();
    int pitch = engine_main_vscreen_pitch();
    if (!vscr) return;
    uint8_t color_table[16];
    color_table[0] = 0;
    color_table[1] = g_charset_color;
    for (int i = 2; i < 16; i++) color_table[i] = g_charset_color_map[i];

    // The first character of a new message kicks off layout — lay out
    // the entire line at xpos/ypos. ScummVM does this incrementally
    // (one glyph per frame) but for the v4 boot path the simpler
    // "draw it all when it begins" mirrors the visual outcome:
    if (g_charset_buf_pos == 0) {
        g_next_left = g_string[0].xpos;
        g_next_top  = g_string[0].ypos;
        if (g_string[0].center) {
            int total_w = 0;
            for (int i = 0; g_charset_buffer[i] != 0; i++) {
                uint8_t c = g_charset_buffer[i];
                if (c == 0xFF) {
                    uint8_t code = g_charset_buffer[i+1];
                    if (code == 1 || code == 2 || code == 3 || code == 8) i += 1;
                    else i += 3;
                    continue;
                }
                total_w += charset_glyph_advance(cs, (char)c);
            }
            g_next_left = g_string[0].xpos - total_w / 2;
        }
    }

    // Process characters until we hit a wait / end.
    while (g_charset_buf_pos < g_charset_buf_len) {
        uint8_t c = g_charset_buffer[g_charset_buf_pos++];
        if (c == 0) {
            // End of message: keep displayed until next stopTalk.
            g_have_msg = 1;
            g_keep_text = false;
            return;
        }
        if (c == 0xFF) {
            uint8_t code = g_charset_buffer[g_charset_buf_pos++];
            switch (code) {
            case 1: // newline
                g_next_left = g_string[0].xpos;
                g_next_top += cs->glyph_height + 1;
                continue;
            case 2: // keep text
                g_have_msg = 0;
                g_keep_text = true;
                return;
            case 3: // wait — yield, but stop emitting until stopTalk
                g_have_msg = 0xFF;
                g_keep_text = false;
                return;
            case 8: // keep text 2
                continue;
            default:
                // 4-byte escape (sound / anim markers)
                g_charset_buf_pos += 2;
                continue;
            }
        }
        charset_draw_char(cs, (char)c, g_next_left, g_next_top,
                          color_table, vscr, pitch);
        g_next_left += charset_glyph_advance(cs, (char)c);
        g_talk_delay += (int)g_vm.globals[VAR_CHARINC];
        if (g_talk_delay > 0) return;
    }

    // Walked off the buffer without a NUL (shouldn't happen — convert
    // null-terminates), but be safe.
    g_have_msg = 1;
    g_keep_text = false;
}

// ---------------------------------------------------------------------------
// decodeParseString — wired into op_print/printEgo
// ---------------------------------------------------------------------------
//
// Mirrors ScummEngine_v5::decodeParseString (script_v5.cpp:3443-3577).
// Sets _string[textSlot] from the SO_xx sub-ops, terminating on
// SO_TEXTSTRING (sub & 0x0F == 15) which calls printString.
void string_decode_parse(VM *vm, int actor_to_print_for) {
    g_actor_to_print_for = actor_to_print_for;

    int textSlot;
    switch (actor_to_print_for) {
    case 252: textSlot = 3; break;
    case 253: textSlot = 2; break;
    case 254: textSlot = 1; break;
    default:  textSlot = 0; break;
    }
    string_load_default(textSlot);
    StringSettings *s = &g_string[textSlot];

    while (true) {
        uint8_t op = vm_fetch_byte(vm);
        if (op == 0xFF) break;
        uint8_t saved = vm->opcode;
        vm->opcode = op;
        switch (op & 0x0F) {
        case 0:    // SO_AT
            s->xpos = (int16_t)vm_get_var_or_word(vm, 0x80);
            s->ypos = (int16_t)vm_get_var_or_word(vm, 0x40);
            s->overhead = false;
            break;
        case 1:    // SO_COLOR
            s->color = (uint8_t)vm_get_var_or_byte(vm, 0x80);
            break;
        case 2:    // SO_CLIPPED
            s->right = (int16_t)vm_get_var_or_word(vm, 0x80);
            break;
        case 3: {  // SO_ERASE — restoreCharsetBg(xpos, xpos+w, ypos, ypos+h)
            int w = vm_get_var_or_word(vm, 0x80);
            int h = vm_get_var_or_word(vm, 0x40);
            (void)w; (void)h;
            // We don't model a separate text VirtScreen yet — there's
            // nothing to erase. Mirrors the "case 3" in
            // script_v5.cpp:3511-3516 short of restoreCharsetBg.
            break;
        }
        case 4:    // SO_CENTER
            s->center = true;
            s->overhead = false;
            break;
        case 6:    // SO_LEFT — v4+: clear center/overhead
            s->center = false;
            s->overhead = false;
            break;
        case 7:    // SO_OVERHEAD
            s->overhead = true;
            break;
        case 8: {  // SO_SAY_VOICE — Loom v4 CD only (ignored here)
            (void)vm_get_var_or_word(vm, 0x80);
            (void)vm_get_var_or_word(vm, 0x40);
            break;
        }
        case 15: { // SO_TEXTSTRING — terminator + in-line message body
            const uint8_t *msg = vm->cur_script_data.data + vm->cur_pc;
            // Skip the string in the bytecode stream so the script
            // continues after it. We recompute the length using the
            // same escape-aware walker as resStrLen.
            int n = vm_skip_string(vm);
            (void)n;
            string_print(textSlot, msg);
            vm->opcode = saved;
            return;
        }
        default:
            platform::log("decodeParseString: unknown sub 0x%02X\n", op & 0x0F);
            break;
        }
        vm->opcode = saved;
    }
    string_save_default(textSlot);
}

// Stubs for name resources we don't load yet.
const uint8_t *string_get_actor_name(int actor)             { (void)actor; return nullptr; }
const uint8_t *string_get_obj_or_actor_name(int id)         { (void)id; return nullptr; }

}  // namespace tsb
