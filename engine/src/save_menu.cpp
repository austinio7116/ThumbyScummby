// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — hold-LB save/load menu.

#include "save_menu.h"
#include "save_backend.h"
#include "platform.h"
#include "scumm/scumm.h"
#include "common/serializer.h"
#include "common/stream.h"

#include <cstring>

namespace tsb {
namespace save_menu {

namespace {

// ---------------------------------------------------------------------------
// 5x7 ASCII font (printable subset 0x20..0x7E).  Each glyph: 5 bytes,
// bit i of byte j = pixel at column j, row i.  Same data as the device
// log overlay; duplicated here so the host build doesn't need to expose
// the device-specific table.
//
// Public-domain font.
// ---------------------------------------------------------------------------
constexpr uint8_t kFont5x7[95][5] = {
	{0,0,0,0,0},          // ' '
	{0,0,0x5F,0,0},       // !
	{0,7,0,7,0},          // "
	{0x14,0x7F,0x14,0x7F,0x14}, // #
	{0x24,0x2A,0x7F,0x2A,0x12}, // $
	{0x23,0x13,8,0x64,0x62},    // %
	{0x36,0x49,0x55,0x22,0x50}, // &
	{0,5,3,0,0},          // '
	{0,0x1C,0x22,0x41,0}, // (
	{0,0x41,0x22,0x1C,0}, // )
	{0x14,8,0x3E,8,0x14}, // *
	{8,8,0x3E,8,8},       // +
	{0,0x50,0x30,0,0},    // ,
	{8,8,8,8,8},          // -
	{0,0x60,0x60,0,0},    // .
	{0x20,0x10,8,4,2},    // /
	{0x3E,0x51,0x49,0x45,0x3E}, // 0
	{0,0x42,0x7F,0x40,0}, // 1
	{0x42,0x61,0x51,0x49,0x46}, // 2
	{0x21,0x41,0x45,0x4B,0x31}, // 3
	{0x18,0x14,0x12,0x7F,0x10}, // 4
	{0x27,0x45,0x45,0x45,0x39}, // 5
	{0x3C,0x4A,0x49,0x49,0x30}, // 6
	{1,0x71,9,5,3},       // 7
	{0x36,0x49,0x49,0x49,0x36}, // 8
	{6,0x49,0x49,0x29,0x1E}, // 9
	{0,0x36,0x36,0,0},    // :
	{0,0x56,0x36,0,0},    // ;
	{0,8,0x14,0x22,0x41}, // <
	{0x14,0x14,0x14,0x14,0x14}, // =
	{0x41,0x22,0x14,8,0}, // >
	{2,1,0x51,9,6},       // ?
	{0x32,0x49,0x79,0x41,0x3E}, // @
	{0x7E,0x11,0x11,0x11,0x7E}, // A
	{0x7F,0x49,0x49,0x49,0x36}, // B
	{0x3E,0x41,0x41,0x41,0x22}, // C
	{0x7F,0x41,0x41,0x22,0x1C}, // D
	{0x7F,0x49,0x49,0x49,0x41}, // E
	{0x7F,9,9,1,1},       // F
	{0x3E,0x41,0x41,0x51,0x32}, // G
	{0x7F,8,8,8,0x7F},    // H
	{0,0x41,0x7F,0x41,0}, // I
	{0x20,0x40,0x41,0x3F,1}, // J
	{0x7F,8,0x14,0x22,0x41}, // K
	{0x7F,0x40,0x40,0x40,0x40}, // L
	{0x7F,2,4,2,0x7F},    // M
	{0x7F,4,8,0x10,0x7F}, // N
	{0x3E,0x41,0x41,0x41,0x3E}, // O
	{0x7F,9,9,9,6},       // P
	{0x3E,0x41,0x51,0x21,0x5E}, // Q
	{0x7F,9,0x19,0x29,0x46}, // R
	{0x46,0x49,0x49,0x49,0x31}, // S
	{1,1,0x7F,1,1},       // T
	{0x3F,0x40,0x40,0x40,0x3F}, // U
	{0x1F,0x20,0x40,0x20,0x1F}, // V
	{0x7F,0x20,0x18,0x20,0x7F}, // W
	{0x63,0x14,8,0x14,0x63}, // X
	{3,4,0x78,4,3},       // Y
	{0x61,0x51,0x49,0x45,0x43}, // Z
	{0,0x7F,0x41,0x41,0}, // [
	{2,4,8,0x10,0x20},    // backslash
	{0,0x41,0x41,0x7F,0}, // ]
	{4,2,1,2,4},          // ^
	{0x40,0x40,0x40,0x40,0x40}, // _
	{0,1,2,4,0},          // `
	{0x20,0x54,0x54,0x54,0x78}, // a
	{0x7F,0x48,0x44,0x44,0x38}, // b
	{0x38,0x44,0x44,0x44,0x20}, // c
	{0x38,0x44,0x44,0x48,0x7F}, // d
	{0x38,0x54,0x54,0x54,0x18}, // e
	{8,0x7E,9,1,2},       // f
	{0x08,0x14,0x54,0x54,0x3C}, // g
	{0x7F,8,4,4,0x78},    // h
	{0,0x44,0x7D,0x40,0}, // i
	{0x20,0x40,0x44,0x3D,0}, // j
	{0x7F,0x10,0x28,0x44,0}, // k
	{0,0x41,0x7F,0x40,0}, // l
	{0x7C,4,0x18,4,0x78}, // m
	{0x7C,8,4,4,0x78},    // n
	{0x38,0x44,0x44,0x44,0x38}, // o
	{0x7C,0x14,0x14,0x14,8}, // p
	{8,0x14,0x14,0x18,0x7C}, // q
	{0x7C,8,4,4,8},       // r
	{0x48,0x54,0x54,0x54,0x20}, // s
	{4,0x3F,0x44,0x40,0x20}, // t
	{0x3C,0x40,0x40,0x20,0x7C}, // u
	{0x1C,0x20,0x40,0x20,0x1C}, // v
	{0x3C,0x40,0x30,0x40,0x3C}, // w
	{0x44,0x28,0x10,0x28,0x44}, // x
	{0x0C,0x50,0x50,0x50,0x3C}, // y
	{0x44,0x64,0x54,0x4C,0x44}, // z
	{0,8,0x36,0x41,0},    // {
	{0,0,0x7F,0,0},       // |
	{0,0x41,0x36,8,0},    // }
	{8,4,8,0x10,8},       // ~
};

void draw_text(int x, int y, const char *str, uint16_t color) {
	for (int col = 0; str[col]; col++) {
		char c = str[col];
		if (c < 0x20 || c > 0x7E) c = '?';
		const uint8_t *glyph = kFont5x7[c - 0x20];
		const int px = x + col * 6;
		for (int gx = 0; gx < 5; gx++) {
			const uint8_t bits = glyph[gx];
			for (int gy = 0; gy < 7; gy++) {
				if (bits & (1 << gy)) {
					tsb::platform::lcd_pixel(px + gx, y + gy, color);
				}
			}
		}
	}
}

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kHilite = 0xFD60;     // amber
constexpr uint16_t kDim = 0x39E7;        // dark grey

constexpr int kMenuItems = 3;
constexpr const char *kLabels[kMenuItems] = { "SAVE", "LOAD", "CANCEL" };
enum { CHOICE_SAVE = 0, CHOICE_LOAD = 1, CHOICE_CANCEL = 2 };

void paint_menu(int sel, bool has_save, const char *status) {
	tsb::platform::lcd_fill(kBlack);
	draw_text(28, 18, "SAVE / LOAD", kWhite);
	for (int i = 0; i < kMenuItems; i++) {
		const bool greyed = (i == CHOICE_LOAD && !has_save);
		uint16_t color = greyed ? kDim : kWhite;
		if (i == sel) color = greyed ? kDim : kHilite;
		const int y = 48 + i * 14;
		const int x = 36;
		if (i == sel) draw_text(24, y, ">", kHilite);
		draw_text(x, y, kLabels[i], color);
	}
	if (status && status[0]) {
		draw_text(8, 110, status, kWhite);
	} else {
		draw_text(2, 110, "A=ok B=cancel", kDim);
	}
	tsb::platform::lcd_present_now();
}

uint32_t s_lb_hold_start_ms = 0;
bool     s_lb_was_held      = false;

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
	int sel = CHOICE_SAVE;
	bool has = save_backend::has_save();
	const char *status = nullptr;

	// Wait for LB to be released first so we don't immediately re-fire.
	{
		tsb::platform::Input in{};
		while (tsb::platform::poll_input(&in) && in.button_lb) {
			paint_menu(sel, has, status);
			tsb::platform::sleep_ms(16);
		}
	}

	while (true) {
		paint_menu(sel, has, status);

		tsb::platform::Input in{};
		if (!tsb::platform::poll_input(&in)) return;  // quit

		if (in.menu_pressed) return;  // hard cancel
		if (in.b_pressed)    return;

		if (in.dpad_up) {
			sel = (sel + kMenuItems - 1) % kMenuItems;
			tsb::platform::sleep_ms(120);
		}
		if (in.dpad_down) {
			sel = (sel + 1) % kMenuItems;
			tsb::platform::sleep_ms(120);
		}

		if (in.a_pressed) {
			if (sel == CHOICE_CANCEL) return;

			if (sel == CHOICE_SAVE) {
				status = "SAVING...";
				paint_menu(sel, has, status);
				bool ok = engine->saveSlot0("Slot 0");
				status = ok ? "SAVED" : "SAVE FAILED";
				has = save_backend::has_save();
				paint_menu(sel, has, status);
				tsb::platform::sleep_ms(900);
				return;
			}

			if (sel == CHOICE_LOAD && has) {
				status = "LOADING...";
				paint_menu(sel, has, status);
				bool ok = engine->loadSlot0();
				status = ok ? "LOADED" : "LOAD FAILED";
				paint_menu(sel, has, status);
				tsb::platform::sleep_ms(900);
				return;
			}
		}

		tsb::platform::sleep_ms(16);
	}
}

}  // namespace save_menu
}  // namespace tsb
