// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — inventory picker overlay.

#include "inventory_picker.h"
#include "platform.h"
#include "mi_font_render.h"
#include "osystem_thumby.h"
#include "scumm/scumm.h"
#include "scumm/verbs.h"
#include "common/system.h"

namespace tsb {
namespace inventory_picker {
namespace {

// MI1 palette (user-confirmed RGB888 → RGB565):
//   inventory  #982390 → 0x9912
//   highlight  #cec760 → 0xCE2C
constexpr uint16_t kWhite  = 0x9912;
constexpr uint16_t kHilite = 0xCE2C;
constexpr uint16_t kDim    = 0x39E7;

constexpr int kBoxX = 0;
constexpr int kBoxY = 60;
constexpr int kBoxW = 128;
constexpr int kBoxH = 60;

struct Entry { int slot_index; char name[24]; };

int gather_inventory(ScummEngine *eng, Entry *out, int max) {
	// Read the engine's authoritative inventory array — _inventory[i]
	// holds the object id of every item the ego actor owns.  Don't
	// look at verb slots (kImageVerbType): those are only populated
	// when the script renders the on-screen inventory grid, which our
	// scene-only redesign doesn't do.
	const uint16 *inv = eng->publicGetInventory();
	const int     num = eng->publicGetNumInventory();
	const int     ego = eng->publicGetEgoVar();
	int n = 0;
	for (int i = 0; i < num && n < max; ++i) {
		const int obj = inv[i];
		if (obj == 0) continue;                    // empty slot
		if (eng->publicGetActorOwner(obj) != ego) continue;  // not held by player
		const byte *name = eng->publicGetObjOrActorName(obj);
		if (!name || !name[0]) continue;
		// MI1 v4 OBCD names are stored inline in the resource and
		// may not be strictly null-terminated for the rendering
		// purposes we want — adjacent bytes (padding / next field)
		// can include 0x2D ('-') or '@' (SCUMM space-pad).  Copy
		// only printable 7-bit ASCII (32..126) and stop at the
		// first '@' (SCUMM end-of-string fill), 0xFF (SCUMM
		// markup escape), or non-printable byte.  Trim trailing
		// spaces too.
		out[n].slot_index = obj;
		int dst = 0;
		for (int j = 0; j < (int)sizeof(out[n].name) - 1 && name[j]; ++j) {
			const byte b = name[j];
			if (b == 0xFF) break;          // SCUMM control-code escape
			if (b == '@')  break;          // SCUMM end-of-string padding
			if (b < 32 || b > 126) break;  // non-printable
			out[n].name[dst++] = (char)b;
		}
		while (dst > 0 && out[n].name[dst - 1] == ' ') --dst;
		out[n].name[dst] = 0;
		if (dst == 0) continue;            // nothing renderable
		++n;
	}
	return n;
}

void paint(OSystem_Thumby *osys, const Entry *entries, int count, int sel) {
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

	tsb::mi_font::draw(kBoxX + 4, kBoxY + 3, "INVENTORY", kHilite);

	if (count == 0) {
		tsb::mi_font::draw(kBoxX + 8, kBoxY + 26, "(empty)", kDim);
		tsb::platform::lcd_present_now();
		return;
	}

	const int max_visible = 6;
	int top = sel - max_visible / 2;
	if (top < 0) top = 0;
	if (top + max_visible > count) top = count - max_visible;
	if (top < 0) top = 0;

	constexpr int kRowX    = kBoxX + 2;
	constexpr int kRowMaxW = kBoxW - 4;
	static uint32_t s_marquee_frame = 0; ++s_marquee_frame;

	for (int i = 0; i < max_visible && top + i < count; i++) {
		const int idx = top + i;
		const int y = kBoxY + 14 + i * 7;
		const uint16_t color = (idx == sel) ? kHilite : kWhite;
		const int text_w = tsb::mi_font::text_width(entries[idx].name);
		const int scroll = (idx == sel)
		    ? tsb::mi_font::marquee_offset(text_w, kRowMaxW, s_marquee_frame)
		    : 0;
		tsb::mi_font::draw_clipped(kRowX - scroll, y, entries[idx].name,
		                           color, kRowX, kRowX + kRowMaxW);
	}

	tsb::platform::lcd_present_now();
}

}  // anonymous

void run(ScummEngine *engine) {
	if (!engine) return;
	OSystem_Thumby *osys = (::g_system != nullptr)
		? static_cast<OSystem_Thumby *>(::g_system) : nullptr;

	Entry entries[20];
	int count = gather_inventory(engine, entries, 20);
	int sel = 0;
	bool prev_up = false, prev_down = false;

	while (true) {
		paint(osys, entries, count, sel);

		tsb::platform::Input in{};
		if (!tsb::platform::poll_input(&in)) return;
		if (in.menu_pressed || in.b_pressed || in.rb_pressed) return;

		if (count == 0) { tsb::platform::sleep_ms(16); continue; }

		const bool up_edge   = in.dpad_up   && !prev_up;
		const bool down_edge = in.dpad_down && !prev_down;
		prev_up   = in.dpad_up;
		prev_down = in.dpad_down;
		if (up_edge)   sel = (sel + count - 1) % count;
		if (down_edge) sel = (sel + 1) % count;

		if (in.a_pressed) {
			// MI1 v5 inventory: items render as kTextVerbType verbs
			// in the panel.  Visible item slots have verbid 101..104,
			// curmode=1.  The slot's resource name is a SCUMM control
			// sequence (0xff 0x06 ...) that resolves to the object
			// name only at draw time — we can't compare names from
			// the resource directly.  Instead, the sel-th picker
			// entry corresponds to the sel-th visible inventory slot
			// (both are in inventory order).
			//
			// Find the sel-th visible inventory verb, then dispatch
			// its verbid via runInputScript directly.  This matches
			// the engine's checkExecVerbs path
			// (verbs.cpp:689 runInputScript(kVerbClickArea,
			// _verbs[over].verbid, code)) but skips findVerbAtPos —
			// we already know the verbid.  The verb-script reads
			// only `val` for kVerbClickArea handling, so _mouse
			// state doesn't need setup.
			int target_verbid = 0;
			int idx = 0;
			for (int v = 1; v < engine->numVerbs(); ++v) {
				const VerbSlot &vs = engine->_verbs[v];
				if (vs.saveid || !vs.curmode || !vs.verbid) continue;
				if (vs.verbid < 101 || vs.verbid > 104) continue;
				if (idx == sel) { target_verbid = vs.verbid; break; }
				++idx;
			}
			if (target_verbid > 0) {
				engine->publicRunInputScript(kVerbClickArea,
				                             target_verbid, 1);
			}
			// Items outside the visible 4-slot window need scroll
			// first — TODO via verbid 107/108 (scroll arrows).
			return;
		}
		tsb::platform::sleep_ms(16);
	}
}

}  // namespace inventory_picker
}  // namespace tsb
