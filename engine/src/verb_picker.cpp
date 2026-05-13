// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — verb picker overlay.

#include "verb_picker.h"
#include "platform.h"
#include "mi_font_render.h"
#include "osystem_thumby.h"
#include "scumm/scumm.h"
#include "scumm/verbs.h"
#include "common/system.h"

namespace tsb {
namespace verb_picker {
namespace {

constexpr uint16_t kBlack  = 0x0000;
// MI1 palette (user-confirmed RGB888 → RGB565):
//   verb/response   #008f00 → 0x0460
//   highlight       #cec760 → 0xCE2C
constexpr uint16_t kWhite  = 0x0460;
constexpr uint16_t kHilite = 0xCE2C;
constexpr uint16_t kDim    = 0x39E7;

constexpr int kBoxX = 0;
constexpr int kBoxY = 60;
constexpr int kBoxW = 128;
constexpr int kBoxH = 60;

// Up to 16 picker entries — covers MI1's 12-verb grid plus the
// occasional Indy-style 16-verb interface.  Each entry knows its
// engine-side _verbs[] index so synthesize_click can target the right
// curRect.  Text is stored as an EXPANDED copy of the rtVerb resource
// (run through convertMessageToString) so SCUMM 0xFF markup like
// variable-substitution and Indy4 dialog markers come out as plain
// readable ASCII rather than getting drawn as 4 px gaps by mi_font.
struct PickerEntry {
	int  slot_index;
	char text[48];
};

// Iterate _verbs[], skip slots that aren't user-clickable (saveid set
// or curmode/verbid 0), collect into out[].  Returns count.
int gather_visible_verbs(ScummEngine *eng, PickerEntry *out, int max) {
	int n = 0;
	for (int v = 1; v < eng->numVerbs() && n < max; ++v) {
		const VerbSlot &vs = eng->_verbs[v];
		if (vs.saveid || !vs.curmode || !vs.verbid) continue;
		// Inventory items live in the same _verbs[] array but as
		// type=kImageVerbType (sprite cells); we list them in the
		// inventory picker, not here.
		if (vs.type != kTextVerbType) continue;
		const byte *txt = eng->getResourceAddress(rtVerb, v);
		if (!txt || txt[0] == 0) continue;
		// Run the engine's SCUMM-markup expansion so 0xFF escape
		// sequences (variable substitution, name-of-object,
		// Indy4-specific 0x7F dialog marker, etc.) become plain
		// ASCII.  Without this, e.g. Indy4 dialog options like
		// "* It begins with..." render as just "*" because the rest
		// of the option text is encoded as a 0xFF reference that
		// mi_font skips.
		byte expanded[64];
		expanded[0] = 0;
		eng->publicConvertMessageToString(txt, expanded, sizeof(expanded));
		// Copy printable bytes into out[n].text, stopping at '@'
		// (SCUMM end-of-string padding) or any non-printable.
		// Skip a leading 0x7F if present — Indy4 dialog options
		// start with that as a "this string has been picked" marker
		// that we don't need to show as a glyph.
		int dst = 0;
		int src_pos = 0;
		if (expanded[0] == 0x7F) src_pos = 1;
		while (dst < (int)sizeof(out[n].text) - 1 && expanded[src_pos]) {
			const byte b = expanded[src_pos++];
			if (b == '@') break;
			if (b < 32 || b > 126) continue;   // drop control bytes silently
			out[n].text[dst++] = (char)b;
		}
		// Trim trailing spaces.
		while (dst > 0 && out[n].text[dst - 1] == ' ') --dst;
		out[n].text[dst] = '\0';
		if (dst == 0) continue;
		out[n].slot_index = v;
		++n;
	}
	return n;
}

void paint(OSystem_Thumby *osys, ScummEngine *eng,
           const PickerEntry *entries, int count, int sel) {
	if (osys) osys->renderSnapshotToFramebuffer();

	tsb::platform::lcd_dim_box(kBoxX, kBoxY, kBoxW, kBoxH);
	for (int x = 0; x < kBoxW; x++) {
		tsb::platform::lcd_pixel(kBoxX + x, kBoxY,             kDim);
		tsb::platform::lcd_pixel(kBoxX + x, kBoxY + kBoxH - 1, kDim);
	}
	for (int y = 0; y < kBoxH; y++) {
		tsb::platform::lcd_pixel(kBoxX,             kBoxY + y, kDim);
		tsb::platform::lcd_pixel(kBoxX + kBoxW - 1, kBoxY + y, kDim);
	}

	const bool dialog = dialog_mode_active(eng);
	tsb::mi_font::draw(kBoxX + 4, kBoxY + 3,
	                   dialog ? "RESPONSE" : "VERB",
	                   kHilite);

	// Show up to 6 entries with a scroll window centred on `sel`.
	const int max_visible = 6;
	int top = sel - max_visible / 2;
	if (top < 0) top = 0;
	if (top + max_visible > count) top = count - max_visible;
	if (top < 0) top = 0;

	// Selection is shown by colour (yellow for selected, white otherwise),
	// so no left-margin marker — gives 2 px margin both sides for text.
	constexpr int kRowX     = kBoxX + 2;
	constexpr int kRowMaxW  = kBoxW - 4;

	static uint32_t s_marquee_frame = 0; ++s_marquee_frame;

	for (int i = 0; i < max_visible && top + i < count; i++) {
		const int idx = top + i;
		const int y = kBoxY + 14 + i * 7;
		const uint16_t color = (idx == sel) ? kHilite : kWhite;
		const int text_w = tsb::mi_font::text_width(entries[idx].text);
		const int scroll = (idx == sel)
		    ? tsb::mi_font::marquee_offset(text_w, kRowMaxW, s_marquee_frame)
		    : 0;
		tsb::mi_font::draw_clipped(kRowX - scroll, y, entries[idx].text,
		                           color, kRowX, kRowX + kRowMaxW);
	}

	// Show count indicator for long lists.
	if (count > max_visible) {
		char buf[16];
		int n = 0;
		buf[n++] = '0' + ((sel + 1) / 10) % 10;
		buf[n++] = '0' + (sel + 1) % 10;
		buf[n++] = '/';
		buf[n++] = '0' + (count / 10) % 10;
		buf[n++] = '0' + count % 10;
		buf[n] = 0;
		tsb::mi_font::draw(kBoxX + kBoxW - 28, kBoxY + 3, buf, kDim);
	}

	tsb::platform::lcd_present_now();
}

}  // anonymous

// 32-bit fingerprint of the currently visible TextVerb set.  Changes
// whenever a verb is added / removed / repositioned — used by the
// "B-to-dismiss" auto-open escape hatch so the picker doesn't re-pop
// while the engine is still in the same verb state.
static uint32_t s_dismissed_fingerprint = 0;

// Track the name of the verb the user most-recently picked from the
// verb picker.  inventory_picker reads this to compose a partial
// sentence ("Use fish ...") in the LCD strip when the engine's own
// sentence-rendering path doesn't fire on our synthesized click.
static char s_last_verb_name[24] = {0};
const char *last_picked_verb_name() { return s_last_verb_name; }

uint32_t verb_set_fingerprint(ScummEngine *engine) {
	if (!engine) return 0;
	uint32_t h = 5381;
	for (int v = 1; v < engine->numVerbs(); ++v) {
		const VerbSlot &vs = engine->_verbs[v];
		if (!vs.curmode || vs.saveid) continue;
		if (vs.type != kTextVerbType) continue;
		h = ((h << 5) + h) ^ (uint32_t)vs.verbid;
		h = ((h << 5) + h) ^ (uint32_t)vs.curRect.top;
		h = ((h << 5) + h) ^ (uint32_t)vs.curRect.left;
	}
	return h;
}

bool dialog_mode_active(ScummEngine *engine) {
	if (!engine) return false;

	// Engine-state predicate.  SCUMM has NO engine-level "in dialog"
	// flag — confirmed by greps of scummvm-upstream/engines/scumm/
	// (no isInDialog / dialogActive / inConversation / _dialog* found).
	// Dialog trees are entirely scripted: the script HIDES the standard
	// 12 interface verbs (SO_VERB_OFF for verbid 1..12), SHOWS the
	// response-option verbs (SO_VERB_NEW + SO_VERB_AT + SO_VERB_NAME_STR
	// + SO_VERB_ON), waits for the user's pick (breakUntil(VAR_VERB ==
	// X)), then RESTORES the standard verbs.  This three-state
	// predicate captures the exact moment when the user is meant to
	// pick:
	//
	//   1. _userPut > 0           input enabled — same gate the engine
	//                             itself uses in checkExecVerbs
	//                             (scummvm-upstream verbs.cpp:698).
	//   2. NO standard verb       no kTextVerbType slot with
	//      visible                verbid 1..12 has curmode == 1.
	//                             Standard verbs being absent is the
	//                             script's "we are NOT in normal verb
	//                             mode" signal.  Used both for dialog
	//                             trees AND for inventory-only / map /
	//                             save-screen modes.
	//   3. SOME other             a kTextVerbType slot with verbid
	//      TextVerb visible       outside 1..12 has curmode == 1 —
	//                             that's a response option spawned by
	//                             the dialog script (or, in the
	//                             unlikely case of a non-dialog
	//                             menu screen, a custom-verb screen
	//                             the user can interact with).
	//
	// Width thresholds, curRect.top positions, the temporal-window
	// edge tracking on setTalkingActor, and the per-slot SO_VERB_NEW
	// flag all turned out to be guesses that broke for various
	// MI1 dialogs (response options can be narrow; pre-spawn timing
	// inverted the temporal model).  Pure state, no heuristics.
	if (engine->userPut() <= 0) return false;

	bool any_nonstandard_visible = false;
	for (int v = 1; v < engine->numVerbs(); ++v) {
		const VerbSlot &vs = engine->_verbs[v];
		if (!vs.curmode || vs.saveid) continue;
		if (vs.type != kTextVerbType) continue;
		if (vs.verbid >= 1 && vs.verbid <= 12) {
			// Standard interface verb is visible → not dialog.
			return false;
		}
		any_nonstandard_visible = true;
	}
	if (!any_nonstandard_visible) return false;

	// Dismiss-fingerprint: when the user just dismissed the picker
	// (B / LB / MENU), suppress re-open until the engine's verb set
	// changes (new dialog appears, response runs, room transitions).
	if (verb_set_fingerprint(engine) == s_dismissed_fingerprint) {
		return false;
	}
	return true;
}

void run(ScummEngine *engine) {
	if (!engine) return;
	OSystem_Thumby *osys = (::g_system != nullptr)
		? static_cast<OSystem_Thumby *>(::g_system) : nullptr;

	PickerEntry entries[16];
	int count = gather_visible_verbs(engine, entries, 16);
	if (count == 0) return;

	int sel = 0;
	bool prev_up = false, prev_down = false;

	while (true) {
		paint(osys, engine, entries, count, sel);

		tsb::platform::Input in{};
		if (!tsb::platform::poll_input(&in)) return;

		if (in.menu_pressed || in.b_pressed || in.lb_pressed) {
			// Snapshot the current verb set so the auto-open path
			// (dialog_mode_active) won't re-fire until the engine
			// changes the verb state — new dialog appears, response
			// script runs, etc.  Without this, a false-positive flag
			// would cause the picker to bounce open every frame.
			s_dismissed_fingerprint = verb_set_fingerprint(engine);
			return;
		}

		const bool up_edge   = in.dpad_up   && !prev_up;
		const bool down_edge = in.dpad_down && !prev_down;
		prev_up   = in.dpad_up;
		prev_down = in.dpad_down;
		if (up_edge)   sel = (sel + count - 1) % count;
		if (down_edge) sel = (sel + 1) % count;

		if (in.a_pressed) {
			const VerbSlot &vs = engine->_verbs[entries[sel].slot_index];
			const bool was_dialog = dialog_mode_active(engine);
			// Remember the verb's text so inventory_picker (or
			// any future overlay) can read it.
			{
				int i = 0;
				for (; i < (int)sizeof(s_last_verb_name) - 1 &&
				       entries[sel].text[i]; ++i)
					s_last_verb_name[i] = entries[sel].text[i];
				s_last_verb_name[i] = 0;
			}
			// Direct verb-script dispatch — same call the engine
			// makes when checkExecVerbs resolves a real mouse
			// click via findVerbAtPos.  We already know the
			// verbid; skip findVerbAtPos.  The verb-script reads
			// only `val` (the verbid) for kVerbClickArea handling
			// — _mouse / _virtualMouse don't need to be set.
			//
			// Synth-click was unreliable: scaled mouse coords in
			// some build paths (textSurfaceMultiplier=2) caused
			// findVerbAtPos to miss / route to wrong virtscreen.
			engine->publicRunInputScript(kVerbClickArea, vs.verbid, 1);
			// Clear the sentence strip after a dialog response so
			// the player doesn't see their just-spoken line still
			// marquee-scrolling while the NPC's reply renders.
			if (osys && was_dialog) {
				osys->captureSentence(nullptr);
				osys->captureNpcQuestion(nullptr);
			}
			// Lock dismiss-fingerprint on the verb-set the user just
			// picked from.  Engine takes a tick or two to consume the
			// click and update the verb set; without this the auto-open
			// sees "dialog still active" on the next frame and re-pops
			// the picker.
			s_dismissed_fingerprint = verb_set_fingerprint(engine);
			return;
		}
		tsb::platform::sleep_ms(16);
	}
}

}  // namespace verb_picker
}  // namespace tsb
