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
constexpr uint16_t kWhite  = 0x57E5;     // mint green — MI1 verb text
constexpr uint16_t kHilite = 0xFFE0;     // yellow — MI1 highlight
constexpr uint16_t kDim    = 0x39E7;

constexpr int kBoxX = 0;
constexpr int kBoxY = 60;
constexpr int kBoxW = 128;
constexpr int kBoxH = 60;

// Up to 16 picker entries — covers MI1's 12-verb grid plus the
// occasional Indy-style 16-verb interface.  Each entry knows its
// engine-side _verbs[] index so synthesize_click can target the right
// curRect.
struct PickerEntry {
	int  slot_index;
	const char *text;
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
		out[n].slot_index = v;
		out[n].text       = reinterpret_cast<const char *>(txt);
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
	// Authoritative engine signal — set by o5_verbOps's SO_VERB_NEW
	// for slots created mid-gameplay (i.e., after _userPut > 0).
	// Boot-time creates (standard 12 verbs, inventory arrows) leave
	// the flag clear, so they're never mistaken for dialog responses.
	for (int v = 1; v < engine->numVerbs(); ++v) {
		const VerbSlot &vs = engine->_verbs[v];
		if (!vs.curmode || vs.saveid) continue;
		if (vs.type != kTextVerbType) continue;
		if (engine->_verbIsDialogResponse[v]) {
			// "Dismissed" fingerprint suppresses re-auto-open.  Cleared
			// once the verb set changes (new dialog enters / current
			// one ends).
			if (verb_set_fingerprint(engine) == s_dismissed_fingerprint) {
				return false;
			}
			return true;
		}
	}
	return false;
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
			// Click in the middle of the verb's source-space curRect —
			// the engine's hotspot detection accepts any pixel inside.
			const int cx = (vs.curRect.left + vs.curRect.right) / 2;
			const int cy = (vs.curRect.top + vs.curRect.bottom) / 2;
			if (osys) osys->synthesizeLeftClick(cx, cy);
			return;
		}
		tsb::platform::sleep_ms(16);
	}
}

}  // namespace verb_picker
}  // namespace tsb
