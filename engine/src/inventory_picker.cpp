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

constexpr uint16_t kWhite  = 0xFFFF;
constexpr uint16_t kHilite = 0xFD60;
constexpr uint16_t kDim    = 0x39E7;

constexpr int kBoxX = 0;
constexpr int kBoxY = 60;
constexpr int kBoxW = 128;
constexpr int kBoxH = 60;

struct Entry { int slot_index; const char *name; };

int gather_inventory(ScummEngine *eng, Entry *out, int max) {
	int n = 0;
	for (int v = 1; v < eng->numVerbs() && n < max; ++v) {
		const VerbSlot &vs = eng->_verbs[v];
		if (vs.saveid || !vs.curmode || !vs.verbid) continue;
		if (vs.type != kImageVerbType) continue;
		const byte *name = eng->publicGetObjOrActorName(vs.verbid);
		if (!name || !name[0]) continue;
		out[n].slot_index = v;
		out[n].name       = reinterpret_cast<const char *>(name);
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

	for (int i = 0; i < max_visible && top + i < count; i++) {
		const int idx = top + i;
		const int y = kBoxY + 14 + i * 7;
		const uint16_t color = (idx == sel) ? kHilite : kWhite;
		if (idx == sel) tsb::mi_font::draw(kBoxX + 2, y, ">", kHilite);
		tsb::mi_font::draw(kBoxX + 9, y, entries[idx].name, color);
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
			const VerbSlot &vs = engine->_verbs[entries[sel].slot_index];
			const int cx = (vs.curRect.left + vs.curRect.right) / 2;
			const int cy = (vs.curRect.top + vs.curRect.bottom) / 2;
			if (osys) osys->synthesizeLeftClick(cx, cy);
			return;
		}
		tsb::platform::sleep_ms(16);
	}
}

}  // namespace inventory_picker
}  // namespace tsb
