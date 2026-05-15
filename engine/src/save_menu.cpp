// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — hold-LB save/load menu.

#include "save_menu.h"
#include "save_backend.h"
#include "config_backend.h"
#include "audio_mix.h"
#include "platform.h"
#include "mi_font_render.h"
#include "osystem_thumby.h"
#include "scumm/scumm.h"
#include "common/serializer.h"
#include "common/stream.h"

#include <cstdio>
#include <cstring>

namespace tsb {
namespace save_menu {

namespace {

inline void draw_text(int x, int y, const char *str, uint16_t color) {
	tsb::mi_font::draw(x, y, str, color);
}

constexpr uint16_t kBlack = 0x0000;
// MI1 palette (RGB888 → RGB565):
//   verb/response  #008f00 → 0x0460
//   highlight      #cec760 → 0xCE2C
constexpr uint16_t kWhite  = 0x0460;
constexpr uint16_t kHilite = 0xCE2C;
constexpr uint16_t kDim    = 0x39E7;

#ifdef THUMBYONE_SLOT_MODE
constexpr int kMenuItems = 8;
constexpr const char *kLabels[kMenuItems] = {
    "SAVE", "LOAD", "VOLUME", "TEXT SIZE", "SPCH FONT", "LOG", "LOBBY", "CANCEL"
};
enum {
    CHOICE_SAVE      = 0,
    CHOICE_LOAD      = 1,
    CHOICE_VOLUME    = 2,
    CHOICE_TEXT_SIZE = 3,
    CHOICE_SPCH_FONT = 4,
    CHOICE_LOG       = 5,
    CHOICE_LOBBY     = 6,
    CHOICE_CANCEL    = 7,
};
// 8 items in 70 px = 8.75 px per row.  Drop the gutter to fit;
// font is 7 px tall so row pitch 8 stays readable.  (Standalone
// build keeps the previous 8-px pitch with 7 items.)
constexpr int kMenuRowPitch = 8;
#else
constexpr int kMenuItems = 7;
constexpr const char *kLabels[kMenuItems] = {
    "SAVE", "LOAD", "VOLUME", "TEXT SIZE", "SPCH FONT", "LOG", "CANCEL"
};
enum {
    CHOICE_SAVE      = 0,
    CHOICE_LOAD      = 1,
    CHOICE_VOLUME    = 2,
    CHOICE_TEXT_SIZE = 3,
    CHOICE_SPCH_FONT = 4,
    CHOICE_LOG       = 5,
    CHOICE_CANCEL    = 6,
};
// Row pitch shrunk from 9 to 8 to fit a 7th menu item inside the same
// 70-px box.  Leaves a 1-px gutter between 7-px font lines.
constexpr int kMenuRowPitch = 8;
#endif

// Two more rows pushed the box down to fit comfortably above the
// sentence strip (kSentenceLcdY = 120).  Box now spans rows 50..119.
constexpr int kBoxX     = 0;
constexpr int kBoxY     = 50;
constexpr int kBoxW     = 128;
constexpr int kBoxH     = 70;

// Range + step for the speech text scale slider.  6 cells (75, 80, 85,
// 90, 95, 100) — matches the volume bar's discrete-cell visual.
constexpr int kTextScaleMin  = 75;
constexpr int kTextScaleMax  = 100;
constexpr int kTextScaleStep = 5;

void paint_menu(OSystem_Thumby *osys, int sel, bool has_save,
                bool can_save, const char *status) {
	// Refresh the scene under the menu — the engine isn't ticking but
	// the cached _staging frame is what the player saw last.  This also
	// repaints the always-visible sentence strip.
	if (osys) osys->renderSnapshotToFramebuffer();

	// Translucent box: dim the scene region under the menu by 50%, then
	// paint a 1-px border for definition.
	tsb::platform::lcd_dim_box(kBoxX, kBoxY, kBoxW, kBoxH);
	for (int x = 0; x < kBoxW; x++) {
		tsb::platform::lcd_pixel(kBoxX + x,         kBoxY,             kDim);
		tsb::platform::lcd_pixel(kBoxX + x,         kBoxY + kBoxH - 1, kDim);
	}
	for (int y = 0; y < kBoxH; y++) {
		tsb::platform::lcd_pixel(kBoxX,             kBoxY + y, kDim);
		tsb::platform::lcd_pixel(kBoxX + kBoxW - 1, kBoxY + y, kDim);
	}

	draw_text(kBoxX + 4, kBoxY + 3, "MENU", kHilite);
	for (int i = 0; i < kMenuItems; i++) {
		const bool greyed = (i == CHOICE_LOAD && !has_save) ||
		                    (i == CHOICE_SAVE && !can_save);
		uint16_t color = greyed ? kDim : kWhite;
		if (i == sel) color = greyed ? kDim : kHilite;
		const int y = kBoxY + 12 + i * kMenuRowPitch;
		draw_text(kBoxX + 8, y, kLabels[i], color);
	}
	// Status sits right-aligned on the SAVE row so it doesn't overlap
	// the menu items below.  Same slot is reused for the persistent
	// "CAN'T SAVE NOW" (locked state) and transient "SAVING..." /
	// "SAVED" / "LOAD FAILED" / etc. as the user works through the
	// menu — all are short enough to clear the SAVE label on the left.
	if (status && status[0]) {
		const int sw = tsb::mi_font::text_width(status);
		const int sx = kBoxX + kBoxW - sw - 4;
		const int sy = kBoxY + 12 + CHOICE_SAVE * kMenuRowPitch;
		draw_text(sx, sy, status, kHilite);
	}

	tsb::platform::lcd_present_now();
}

uint32_t s_lb_hold_start_ms = 0;
bool     s_lb_was_held      = false;

void paint_volume(OSystem_Thumby *osys, int level) {
	if (osys) osys->renderSnapshotToFramebuffer();

	tsb::platform::lcd_dim_box(kBoxX, kBoxY, kBoxW, kBoxH);
	for (int x = 0; x < kBoxW; x++) {
		tsb::platform::lcd_pixel(kBoxX + x,         kBoxY,             kDim);
		tsb::platform::lcd_pixel(kBoxX + x,         kBoxY + kBoxH - 1, kDim);
	}
	for (int y = 0; y < kBoxH; y++) {
		tsb::platform::lcd_pixel(kBoxX,             kBoxY + y, kDim);
		tsb::platform::lcd_pixel(kBoxX + kBoxW - 1, kBoxY + y, kDim);
	}

	draw_text(kBoxX + 4, kBoxY + 3, "VOLUME", kHilite);

	// Numeric readout.
	char num[8];
	std::snprintf(num, sizeof(num), "%d", level);
	draw_text(kBoxX + kBoxW - 18, kBoxY + 3, num, kWhite);

	// Bar: kAudioMixVolumeMax cells.  Filled cells in white, empty in
	// dim grey, current cell in highlight yellow.  Bar lives in the
	// middle of the box with a 4 px margin per side.
	constexpr int kBarX  = 4;
	constexpr int kBarY  = 24;
	constexpr int kBarW  = 120;
	constexpr int kBarH  = 8;
	const int cells     = kAudioMixVolumeMax + 1;          // 0..max inclusive
	const int cell_w    = kBarW / cells;
	const int bar_total = cell_w * cells;
	const int bar_origin_x = kBoxX + (kBoxW - bar_total) / 2;
	for (int i = 0; i < cells; i++) {
		const int x0 = bar_origin_x + i * cell_w;
		const uint16_t fill = (i == level)
		                          ? kHilite
		                          : (i <= level ? kWhite : kDim);
		for (int yy = 0; yy < kBarH; yy++) {
			for (int xx = 1; xx < cell_w - 1; xx++) {
				tsb::platform::lcd_pixel(x0 + xx, kBoxY + kBarY + yy, fill);
			}
		}
	}
	(void)kBarX; (void)kBarY;  // kept for layout reference

	// Hint text.
	draw_text(kBoxX + 4, kBoxY + 38, "L/R adjust", kDim);
	draw_text(kBoxX + 4, kBoxY + 47, "A/B accept", kDim);

	tsb::platform::lcd_present_now();
}

void run_volume(OSystem_Thumby *osys) {
	int level = audio_mix_get_volume();
	bool prev_left = false, prev_right = false;
	while (true) {
		paint_volume(osys, level);

		tsb::platform::Input in{};
		if (!tsb::platform::poll_input(&in)) return;
		if (in.menu_pressed || in.b_pressed || in.a_pressed) {
			// Persist on exit (also persisted live below — this is the
			// final sync after any rapid adjustments).
			config_backend::save_volume(level);
			return;
		}

		const bool left_edge  = in.dpad_left  && !prev_left;
		const bool right_edge = in.dpad_right && !prev_right;
		prev_left  = in.dpad_left;
		prev_right = in.dpad_right;
		if (left_edge && level > 0) {
			--level;
			audio_mix_set_volume(level);
			config_backend::save_volume(level);
		}
		if (right_edge && level < kAudioMixVolumeMax) {
			++level;
			audio_mix_set_volume(level);
			config_backend::save_volume(level);
		}
		tsb::platform::sleep_ms(16);
	}
}

// Reuses the volume-bar visual.  Cells map 75 → 0 .. 100 → 5.
void paint_text_scale(OSystem_Thumby *osys, int pct) {
	if (osys) osys->renderSnapshotToFramebuffer();

	tsb::platform::lcd_dim_box(kBoxX, kBoxY, kBoxW, kBoxH);
	for (int x = 0; x < kBoxW; x++) {
		tsb::platform::lcd_pixel(kBoxX + x,         kBoxY,             kDim);
		tsb::platform::lcd_pixel(kBoxX + x,         kBoxY + kBoxH - 1, kDim);
	}
	for (int y = 0; y < kBoxH; y++) {
		tsb::platform::lcd_pixel(kBoxX,             kBoxY + y, kDim);
		tsb::platform::lcd_pixel(kBoxX + kBoxW - 1, kBoxY + y, kDim);
	}

	draw_text(kBoxX + 4, kBoxY + 3, "TEXT SIZE", kHilite);

	char num[8];
	std::snprintf(num, sizeof(num), "%d%%", pct);
	draw_text(kBoxX + kBoxW - 24, kBoxY + 3, num, kWhite);

	constexpr int kBarY = 24;
	constexpr int kBarW = 120;
	constexpr int kBarH = 8;
	const int cells     = ((kTextScaleMax - kTextScaleMin) / kTextScaleStep) + 1;
	const int level     = (pct - kTextScaleMin) / kTextScaleStep;
	const int cell_w    = kBarW / cells;
	const int bar_total = cell_w * cells;
	const int bar_origin_x = kBoxX + (kBoxW - bar_total) / 2;
	for (int i = 0; i < cells; i++) {
		const int x0 = bar_origin_x + i * cell_w;
		const uint16_t fill = (i == level)
		                          ? kHilite
		                          : (i <= level ? kWhite : kDim);
		for (int yy = 0; yy < kBarH; yy++) {
			for (int xx = 1; xx < cell_w - 1; xx++) {
				tsb::platform::lcd_pixel(x0 + xx, kBoxY + kBarY + yy, fill);
			}
		}
	}

	draw_text(kBoxX + 4, kBoxY + 38, "L/R adjust", kDim);
	draw_text(kBoxX + 4, kBoxY + 47, "A/B accept", kDim);

	tsb::platform::lcd_present_now();
}

void run_text_scale(OSystem_Thumby *osys) {
	if (!osys) return;
	int pct = osys->getSpeechScalePct();
	bool prev_left = false, prev_right = false;
	while (true) {
		paint_text_scale(osys, pct);

		tsb::platform::Input in{};
		if (!tsb::platform::poll_input(&in)) return;
		if (in.menu_pressed || in.b_pressed || in.a_pressed) {
			config_backend::save_text_scale_pct(pct);
			return;
		}

		const bool left_edge  = in.dpad_left  && !prev_left;
		const bool right_edge = in.dpad_right && !prev_right;
		prev_left  = in.dpad_left;
		prev_right = in.dpad_right;
		if (left_edge && pct > kTextScaleMin) {
			pct -= kTextScaleStep;
			osys->setSpeechScalePct(pct);
			config_backend::save_text_scale_pct(pct);
		}
		if (right_edge && pct < kTextScaleMax) {
			pct += kTextScaleStep;
			osys->setSpeechScalePct(pct);
			config_backend::save_text_scale_pct(pct);
		}
		tsb::platform::sleep_ms(16);
	}
}

void paint_speech_font(OSystem_Thumby *osys, bool use_mi) {
	if (osys) osys->renderSnapshotToFramebuffer();

	tsb::platform::lcd_dim_box(kBoxX, kBoxY, kBoxW, kBoxH);
	for (int x = 0; x < kBoxW; x++) {
		tsb::platform::lcd_pixel(kBoxX + x,         kBoxY,             kDim);
		tsb::platform::lcd_pixel(kBoxX + x,         kBoxY + kBoxH - 1, kDim);
	}
	for (int y = 0; y < kBoxH; y++) {
		tsb::platform::lcd_pixel(kBoxX,             kBoxY + y, kDim);
		tsb::platform::lcd_pixel(kBoxX + kBoxW - 1, kBoxY + y, kDim);
	}

	draw_text(kBoxX + 4, kBoxY + 3, "SPEECH FONT", kHilite);

	// Two options laid out side-by-side; selected is highlighted.
	constexpr int kRowY  = 28;
	const int half_w = kBoxW / 2;
	uint16_t col_scumm = use_mi ? kDim    : kHilite;
	uint16_t col_mi    = use_mi ? kHilite : kDim;
	draw_text(kBoxX + 12,           kBoxY + kRowY, "SCUMM", col_scumm);
	draw_text(kBoxX + half_w + 8,   kBoxY + kRowY, "MI",    col_mi);

	draw_text(kBoxX + 4, kBoxY + 47, "L/R toggle  A/B accept", kDim);

	tsb::platform::lcd_present_now();
}

// --------------------------------------------------------------------
// Log viewer — scrollable read-only view of platform_pico's log ring.
// --------------------------------------------------------------------
constexpr int kLogVisibleRows  = 14;   // ~9 px per row × 14 ≈ 126 px
constexpr int kLogRowPitch     = 9;
constexpr int kLogMaxLineChars = 22;   // matches kLogCols + NUL

void paint_log(int scroll, int total) {
	tsb::platform::lcd_fill(kBlack);

	// Title bar.
	draw_text(2, 0, "LOG", kHilite);
	// Show position indicator: e.g. "12/32".
	char pos[16];
	std::snprintf(pos, sizeof(pos), "%d-%d/%d",
	              scroll + 1,
	              (scroll + kLogVisibleRows < total) ? (scroll + kLogVisibleRows) : total,
	              total);
	draw_text(128 - tsb::mi_font::text_width(pos) - 2, 0, pos, kDim);

	// Lines.
	for (int row = 0; row < kLogVisibleRows; ++row) {
		const int idx = scroll + row;
		if (idx >= total) break;
		char line[kLogMaxLineChars];
		tsb::platform::log_history_get(idx, line, sizeof(line));
		// Newest line gets highlighted.
		const uint16_t col = (idx == total - 1) ? kHilite : kWhite;
		draw_text(0, 12 + row * kLogRowPitch, line, col);
	}

	tsb::platform::lcd_present_now();
}

void run_log(OSystem_Thumby *osys) {
	(void)osys;
	const int total = tsb::platform::log_history_count();
	// Start showing the newest page.
	int scroll = (total > kLogVisibleRows) ? (total - kLogVisibleRows) : 0;
	bool prev_up = false, prev_down = false;
	while (true) {
		paint_log(scroll, total);

		tsb::platform::Input in{};
		if (!tsb::platform::poll_input(&in)) return;
		if (in.menu_pressed || in.b_pressed || in.a_pressed) return;

		const bool up_edge   = in.dpad_up   && !prev_up;
		const bool down_edge = in.dpad_down && !prev_down;
		prev_up   = in.dpad_up;
		prev_down = in.dpad_down;
		if (up_edge   && scroll > 0)                                     --scroll;
		if (down_edge && scroll + kLogVisibleRows < total)               ++scroll;
		// LEFT/RIGHT: page jumps for faster review.
		if (in.dpad_left)  scroll = (scroll > kLogVisibleRows) ? (scroll - kLogVisibleRows) : 0;
		if (in.dpad_right) {
			scroll += kLogVisibleRows;
			if (scroll + kLogVisibleRows > total)
				scroll = (total > kLogVisibleRows) ? (total - kLogVisibleRows) : 0;
		}
		tsb::platform::sleep_ms(16);
	}
}

// --------------------------------------------------------------------
// Slot picker — 2×2 grid of save slots with thumbnail previews.
// --------------------------------------------------------------------
enum class SlotMode { SAVE, LOAD };

void blit_thumb(const uint16_t *src, int srcW, int srcH, int dstX, int dstY) {
	// Simple uint16->lcd_pixel blit.  src may be nullptr (empty slot)
	// in which case we just clear the area.
	for (int y = 0; y < srcH; ++y) {
		for (int x = 0; x < srcW; ++x) {
			const uint16_t c = src ? src[y * srcW + x] : 0x0000;
			tsb::platform::lcd_pixel(dstX + x, dstY + y, c);
		}
	}
}

void paint_slot_picker(SlotMode mode, int sel,
                       const tsb::save_backend::SlotInfo *slots) {
	tsb::platform::lcd_fill(kBlack);

	const char *title = (mode == SlotMode::SAVE) ? "SAVE TO..." : "LOAD FROM...";
	draw_text(2, 0, title, kHilite);

	constexpr int kCellW = 64;
	constexpr int kThumbW = tsb::save_backend::kThumbW;
	constexpr int kThumbH = tsb::save_backend::kThumbH;
	// 36 thumb + 7 line1 + 1 gap + 7 line2 + 1 bottom-pad = 52 px tall.
	// Two rows = 104 px starting at kHeaderH=9 → ends at y=113, leaves
	// 6 px clear of the sentence strip at y=120.  Previous 64 px cells
	// pushed row 2 past y=120 and overlapped the sentence strip.
	constexpr int kCellH = kThumbH + 16;
	constexpr int kHeaderH = 9;   // title takes top 8 px; cells start at 9

	for (int s = 0; s < tsb::save_backend::kNumSlots; ++s) {
		const int cx = (s % 2) * kCellW;
		const int cy = kHeaderH + (s / 2) * kCellH;

		// Thumbnail (or blank).
		blit_thumb(slots[s].thumb, kThumbW, kThumbH, cx, cy);

		// Label area below thumbnail (24 px tall).  Hint string +
		// "SLOT N" line.  Empty slot: show "EMPTY".
		char line1[24], line2[24];
		std::snprintf(line1, sizeof(line1), "SLOT %d", s + 1);
		if (slots[s].occupied) {
			// Use the stored hint as line 2.  Truncate to fit.
			std::strncpy(line2, slots[s].hint, sizeof(line2) - 1);
			line2[sizeof(line2) - 1] = '\0';
		} else {
			std::strcpy(line2, "EMPTY");
		}

		const bool greyed = (mode == SlotMode::LOAD) && !slots[s].occupied;
		const uint16_t col = greyed ? kDim : kWhite;
		draw_text(cx + 1, cy + kThumbH + 1,  line1, col);
		draw_text(cx + 1, cy + kThumbH + 9,  line2, col);

		// Selection border on the focused cell.
		if (s == sel) {
			for (int x = 0; x < kCellW; ++x) {
				tsb::platform::lcd_pixel(cx + x,             cy,             kHilite);
				tsb::platform::lcd_pixel(cx + x,             cy + kCellH - 1, kHilite);
			}
			for (int y = 0; y < kCellH; ++y) {
				tsb::platform::lcd_pixel(cx,                 cy + y, kHilite);
				tsb::platform::lcd_pixel(cx + kCellW - 1,    cy + y, kHilite);
			}
		}
	}

	tsb::platform::lcd_present_now();
}

// RAII guard so every return path from run_slot_picker frees any
// transient thumbnail buffers the backend handed back through
// SlotInfo.thumb.  Flash / host backends have nothing to free;
// the FatFs backend frees its 4×4608 B heap allocations.
struct SlotInfoGuard {
	tsb::save_backend::SlotInfo *info;
	~SlotInfoGuard() { tsb::save_backend::release_slots(info); }
};

// Returns 0..kNumSlots-1 on confirm, -1 on cancel.
int run_slot_picker(SlotMode mode) {
	tsb::save_backend::SlotInfo slots[tsb::save_backend::kNumSlots];
	tsb::save_backend::enumerate_slots(slots);
	SlotInfoGuard guard{slots};
	int sel = 0;
	bool prev_up = false, prev_down = false, prev_left = false, prev_right = false;
	bool prev_a = false;

	// Drain any held A from the parent menu.
	{
		tsb::platform::Input in{};
		while (tsb::platform::poll_input(&in) && in.a_pressed)
			tsb::platform::sleep_ms(16);
	}

	while (true) {
		paint_slot_picker(mode, sel, slots);

		tsb::platform::Input in{};
		if (!tsb::platform::poll_input(&in)) return -1;
		if (in.menu_pressed || in.b_pressed) return -1;

		const bool up_edge    = in.dpad_up    && !prev_up;
		const bool down_edge  = in.dpad_down  && !prev_down;
		const bool left_edge  = in.dpad_left  && !prev_left;
		const bool right_edge = in.dpad_right && !prev_right;
		const bool a_edge     = in.a_pressed  && !prev_a;
		prev_up    = in.dpad_up;
		prev_down  = in.dpad_down;
		prev_left  = in.dpad_left;
		prev_right = in.dpad_right;
		prev_a     = in.a_pressed;

		if (up_edge   && sel >= 2) sel -= 2;
		if (down_edge && sel <= 1) sel += 2;
		if (left_edge  && (sel % 2) == 1) sel -= 1;
		if (right_edge && (sel % 2) == 0) sel += 1;

		if (a_edge) {
			// Block LOAD on empty slot; SAVE is always allowed.
			if (mode == SlotMode::LOAD && !slots[sel].occupied)
				continue;
			return sel;
		}
		tsb::platform::sleep_ms(16);
	}
}

void run_speech_font(OSystem_Thumby *osys) {
	if (!osys) return;
	bool use_mi = osys->getUseMiFontForSpeech();
	bool prev_left = false, prev_right = false;
	while (true) {
		paint_speech_font(osys, use_mi);

		tsb::platform::Input in{};
		if (!tsb::platform::poll_input(&in)) return;
		if (in.menu_pressed || in.b_pressed || in.a_pressed) {
			config_backend::save_use_mi_font(use_mi);
			return;
		}

		const bool left_edge  = in.dpad_left  && !prev_left;
		const bool right_edge = in.dpad_right && !prev_right;
		prev_left  = in.dpad_left;
		prev_right = in.dpad_right;
		if (left_edge && use_mi) {
			use_mi = false;
			osys->setUseMiFontForSpeech(use_mi);
			config_backend::save_use_mi_font(use_mi);
		}
		if (right_edge && !use_mi) {
			use_mi = true;
			osys->setUseMiFontForSpeech(use_mi);
			config_backend::save_use_mi_font(use_mi);
		}
		tsb::platform::sleep_ms(16);
	}
}

}  // anonymous

bool maybe_trigger(bool lb_held, bool is_player_in_control) {
	const uint32_t now = tsb::platform::millis();
	if (!is_player_in_control || !lb_held) {
		s_lb_was_held = false;
		s_lb_hold_start_ms = 0;
		return false;
	}
	if (!s_lb_was_held) {
		s_lb_was_held = true;
		s_lb_hold_start_ms = now;
		return false;
	}
	const uint32_t elapsed = now - s_lb_hold_start_ms;
	if (elapsed >= 800u) {
		// Reset so re-entering the loop doesn't immediately re-trigger.
		s_lb_was_held = false;
		s_lb_hold_start_ms = 0;
		return true;
	}
	return false;
}

void run(ScummEngine *engine) {
	if (!engine) return;
	OSystem_Thumby *osys = (::g_system != nullptr)
		? static_cast<OSystem_Thumby *>(::g_system)
		: nullptr;
	int sel = CHOICE_SAVE;
	// Update `has_any` whenever the slot table might have changed
	// (after a save).  Used to grey out the LOAD row when every slot
	// is empty.
	auto compute_has_any = []() {
		for (int s = 0; s < tsb::save_backend::kNumSlots; ++s)
			if (save_backend::has_save(s)) return true;
		return false;
	};
	bool has = compute_has_any();
	// Snapshot the in-control state ONCE at menu open.  Avoids the
	// SAVE row flickering between greyed and active if a script
	// toggles _userPut while the menu is open.  Matches the original
	// SCUMM verb panel which checked save-allowed at the moment the
	// panel was drawn, not at every redraw.
	const bool can_save = engine->publicIsPlayerInControl();
	const char *status = can_save ? nullptr : "CAN'T SAVE NOW";
	bool prev_up = false, prev_down = false;

	// Wait for LB to be released first so we don't immediately re-fire.
	{
		tsb::platform::Input in{};
		while (tsb::platform::poll_input(&in) && in.button_lb) {
			paint_menu(osys, sel, has, can_save, status);
			tsb::platform::sleep_ms(16);
		}
	}

	while (true) {
		paint_menu(osys, sel, has, can_save, status);

		tsb::platform::Input in{};
		if (!tsb::platform::poll_input(&in)) return;  // quit

		if (in.menu_pressed) return;  // hard cancel
		if (in.b_pressed)    return;

		// Edge-only navigation — Input struct has no dpad press edges,
		// so track our own.  Without this, a normal ~200 ms press
		// advances the selection two or three slots before the user
		// can release.
		const bool up_edge   = in.dpad_up   && !prev_up;
		const bool down_edge = in.dpad_down && !prev_down;
		prev_up   = in.dpad_up;
		prev_down = in.dpad_down;
		if (up_edge)   sel = (sel + kMenuItems - 1) % kMenuItems;
		if (down_edge) sel = (sel + 1) % kMenuItems;

		if (in.a_pressed) {
			if (sel == CHOICE_CANCEL) return;

			if (sel == CHOICE_SAVE) {
				if (!can_save) {
					tsb::platform::sleep_ms(120);
					continue;
				}
				const int slot = run_slot_picker(SlotMode::SAVE);
				if (slot < 0) continue;  // cancelled — back to main menu

				// Capture a thumbnail of the current game scene.  Must
				// happen BEFORE paint_menu redraws because that re-emits
				// the snapshot atop the framebuffer (still safe — we read
				// the cached _staging, not the LCD framebuffer).
				static uint16_t s_thumb[tsb::save_backend::kThumbW *
				                        tsb::save_backend::kThumbH];
				if (osys) {
					osys->captureSlotThumbnail(s_thumb,
					                           tsb::save_backend::kThumbW,
					                           tsb::save_backend::kThumbH);
				} else {
					std::memset(s_thumb, 0, sizeof(s_thumb));
				}

				char hint[tsb::save_backend::kHintBytes];
				std::snprintf(hint, sizeof(hint), "Room %d",
				              engine->publicCurrentRoom());

				char desc[24];
				std::snprintf(desc, sizeof(desc), "Slot %d", slot + 1);

				status = "SAVING...";
				paint_menu(osys, sel, has, can_save, status);
				bool ok = engine->saveSlot(slot, desc, s_thumb, hint);
				status = ok ? "SAVED" : "SAVE FAILED";
				has = compute_has_any();
				paint_menu(osys, sel, has, can_save, status);
				tsb::platform::sleep_ms(900);
				return;
			}

			if (sel == CHOICE_LOAD && has) {
				const int slot = run_slot_picker(SlotMode::LOAD);
				if (slot < 0) continue;
				status = "LOADING...";
				paint_menu(osys, sel, has, can_save, status);
				bool ok = engine->loadSlot(slot);
				if (ok) {
					// scummvm v5 saveload restores _grabbedCursor +
					// _cursor.* fields but never re-uploads the cursor
					// sprite to OSystem for non-Mac platforms — so on
					// device the crosshair vanishes after load.  Two-
					// step refresh:
					//   1. ask the engine to re-emit (works on host).
					//   2. fall back to a hardcoded crosshair (covers
					//      device, where step 1 alone leaves the
					//      sprite invisible — root cause unclear but
					//      this guarantees a visible pointer).
					engine->publicRefreshCursor();
					if (osys) osys->forceVisibleCrosshairCursor();
				}
				status = ok ? "LOADED" : "LOAD FAILED";
				paint_menu(osys, sel, has, can_save, status);
				tsb::platform::sleep_ms(900);
				return;
			}

			// All three sub-menus need to drain the A-press edge first
			// so the sub-screen doesn't immediately treat it as an exit.
			auto drain_a = [&]() {
				tsb::platform::Input drain{};
				while (tsb::platform::poll_input(&drain) && drain.a_pressed)
					tsb::platform::sleep_ms(16);
			};

			if (sel == CHOICE_VOLUME) {
				drain_a();
				run_volume(osys);
				continue;
			}
			if (sel == CHOICE_TEXT_SIZE) {
				drain_a();
				run_text_scale(osys);
				continue;
			}
			if (sel == CHOICE_SPCH_FONT) {
				drain_a();
				run_speech_font(osys);
				continue;
			}
			if (sel == CHOICE_LOG) {
				drain_a();
				run_log(osys);
				continue;
			}
#ifdef THUMBYONE_SLOT_MODE
			if (sel == CHOICE_LOBBY) {
				// Return to ThumbyOne lobby.  Platform layer
				// handles unmount + watchdog-scratch handoff;
				// the call doesn't return.
				tsb::platform::lobby_handoff();
				continue;  // unreachable
			}
#endif
		}

		tsb::platform::sleep_ms(16);
	}
}

}  // namespace save_menu
}  // namespace tsb
