// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — text / string pipeline.
//
// Mirrors the parts of scummvm-upstream/engines/scumm/{string,charset}.cpp
// that the v4/v5 print path uses:
//   - _string[N] settings (xpos/ypos/color/center/charset)
//   - decodeParseString -> printString -> drawString -> charset->printChar
//   - convertMessageToString \xFF-escape resolution
//   - actorTalk / displayDialog talk loop
//
// We don't model multiple VirtScreens yet — text is rendered straight
// onto the main viewport (vscreen_main).

#pragma once

#include "types.h"

namespace tsb {

// SCUMM has six text "settings": [0]=actor talk, [1]=verb name,
// [2]=debug, [3]=showMessageDialog. v4/v5 use 0..3.
constexpr int NUM_STRING_SLOTS = 4;

struct StringSettings {
    int16_t  xpos;
    int16_t  ypos;
    int16_t  right;       // wrap-clip column
    uint8_t  color;
    uint8_t  charset;
    bool     center;
    bool     overhead;
    bool     no_talk_anim;
    int16_t  height;      // v3 stores SO_LEFT word here
};

void string_init();

// Reset _string[slot] to its saved-default. Mirrors loadDefault().
void string_load_default(int slot);
// Save current _string[slot] state as the default. Mirrors saveDefault().
void string_save_default(int slot);

StringSettings *string_get(int slot);

// Mirrors ScummEngine::printString (string.cpp:57-124). m == slot. For
// slot 0 (actor talk) this delegates to actorTalk; for 1/2/3 it goes
// through drawString / debugMessage / showMessageDialog. We support
// 0 and 1 (talk + verb-name draw) for the v4 boot path.
void string_print(int slot, const uint8_t *msg);

// Mirrors ScummEngine::actorTalk (actor.cpp:3466). Sets up _haveMsg /
// _talkDelay / _charsetColor / _charsetBufPos based on the actor and
// the message; the per-frame engine_tick driver then advances the
// charset cursor.
void string_actor_talk(int actor_to_print_for, const uint8_t *msg);

// Mirrors ScummEngine::displayDialog (string.cpp:1033-1331). Called
// once per engine_tick when a talk message is active. Decrements
// _talkDelay; when zero, prints the next text character; when the
// message ends, clears VAR_HAVE_MSG.
void string_tick();

// Stop any active talk. Mirrors ScummEngine::stopTalk (actor.cpp:3564).
void string_stop_talk();

// Mirrors ScummEngine::convertMessageToString (string.cpp:1573-1742).
// Resolves the \xFF\xN escape codes (newline, wait, var-substitute,
// verb name, actor name, string slot). Returns total bytes written.
int  string_convert_message(const uint8_t *src, uint8_t *dst, int dst_size);

// Used by op_print/op_printEgo. Routes _actorToPrintStrFor to the
// matching textSlot, then runs the SO_xx sub-op loop and finally
// printString on the in-line message at the script's PC.
void string_decode_parse(struct VM *vm, int actor_to_print_for);

// Read a v4 actor name resource from rtActorName. We don't yet load
// these; returns nullptr.
const uint8_t *string_get_actor_name(int actor);

// Read a v4 object/actor name (used by \xFF\x06 substitution).
const uint8_t *string_get_obj_or_actor_name(int id);

// Slot for "talk in progress" — read by op_wait sub-op 2 and by
// engine_tick to drive displayDialog.
bool     string_have_message();
int      string_talk_delay();
int      string_get_talking_actor();
void     string_set_keep_text(bool on);

// Set the 16-byte _charsetColorMap. Mirrors o5_cursorCommand sub 14
// (script_v5.cpp:932-937) which populates _charsetData[0..15]. Used by
// >=2bpp glyphs.
void     string_set_charset_colormap(const uint8_t *table, int n);

}  // namespace tsb
