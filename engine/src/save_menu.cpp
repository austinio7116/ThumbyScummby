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

constexpr int kMenuItems = 6;
constexpr const char *kLabels[kMenuItems] = {
    "SAVE", "LOAD", "VOLUME", "TEXT SIZE", "SPCH FONT", "CANCEL"
};
enum {
    CHOICE_SAVE      = 0,
    CHOICE_LOAD      = 1,
    CHOICE_VOLUME    = 2,
    CHOICE_TEXT_SIZE = 3,
    CHOICE_SPCH_FONT = 4,
    CHOICE_CANCEL    = 5,
};

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
		const int y = kBoxY + 12 + i * 9;
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
		const int sy = kBoxY + 12 + CHOICE_SAVE * 9;
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
	bool has = save_backend::has_save();
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
					// Original SCUMM scripts disabled the SAVE verb in
					// these states — match that and decline.  No status
					// flicker; the persistent "CAN'T SAVE NOW" stays.
					tsb::platform::sleep_ms(120);
					continue;
				}
				status = "SAVING...";
				paint_menu(osys, sel, has, can_save, status);
				bool ok = engine->saveSlot0("Slot 0");
				status = ok ? "SAVED" : "SAVE FAILED";
				has = save_backend::has_save();
				paint_menu(osys, sel, has, can_save, status);
				tsb::platform::sleep_ms(900);
				return;
			}

			if (sel == CHOICE_LOAD && has) {
				status = "LOADING...";
				paint_menu(osys, sel, has, can_save, status);
				bool ok = engine->loadSlot0();
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
		}

		tsb::platform::sleep_ms(16);
	}
}

}  // namespace save_menu
}  // namespace tsb
