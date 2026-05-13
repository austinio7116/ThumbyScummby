// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — inventory picker overlay.
//
// Layout — scene-only redesign (sentence strip always visible at LCD
// rows 120..127, so the picker box ends at y=119):
//
//   y=0..7    Title bar:  "INVENTORY  N/M"  (sel index / count)
//   y=10..99  3×3 grid of 42×30 cells (1 px outer margin x, no margin y)
//   y=102..119  Selected-item name strip (marquee if needed)
//   y=120..127  Sentence strip (owned by present(), untouched here)
//
// Cell content:
//   Outline 1 px (kHilite if selected, kDim otherwise)
//   Inside 40×28:
//     - If an icon thumbnail is cached for the item, blit it centred.
//     - Else render the item name centred (single short line — names
//       longer than ~7 chars get truncated; the bottom strip carries
//       the full marquee'd name).
//
// Scrolling: row-wise smooth scroll.  Selection moves cell-by-cell with
// d-pad (no wrap).  When the selection crosses the bottom visible row,
// the grid scrolls up by one row.  Header shows "I N/M".
//
// Phase 1 (this revision): icon cache stays empty — every cell renders
// the text-in-cell fallback.  The captureItemIcon() hook is the
// designated extension point for Phase 2 (decoding rtVerb / rtRoom OBIM
// data into a 36×24 RGB565 thumbnail).

#include "inventory_picker.h"
#include "platform.h"
#include "mi_font_render.h"
#include "osystem_thumby.h"
#include "scumm/scumm.h"
#include "scumm/verbs.h"
#include "common/system.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tsb {
namespace inventory_picker {
namespace {

// MI1 palette (user-confirmed RGB888 → RGB565):
//   inventory  #982390 → 0x9912
//   highlight  #cec760 → 0xCE2C
constexpr uint16_t kWhite  = 0x9912;
constexpr uint16_t kHilite = 0xCE2C;
constexpr uint16_t kDim    = 0x39E7;
constexpr uint16_t kBlack  = 0x0000;

// Grid geometry — sized so 3 columns × 42 px = 126, leaving a 1 px
// margin on each side; 3 rows × 30 px = 90, fitting above the name
// strip at y=102.
constexpr int kGridCols   = 3;
constexpr int kGridRows   = 3;
constexpr int kCellW      = 42;
constexpr int kCellH      = 30;
constexpr int kGridX      = 1;
constexpr int kGridY      = 10;
constexpr int kVisibleCells = kGridCols * kGridRows;

// Icon area = cell interior minus 1 px border on each side.  We
// stretch icons to fill the whole area (no aspect-fit padding) so the
// 32×16 / 48×24 OBIM source ends up filling the cell instead of
// sitting in a tiny letterbox.  Slight horizontal squash is fine —
// the cell aspect is closer to the source aspect than the previous
// 1.67 letterbox was anyway.
constexpr int kIconW      = 40;
constexpr int kIconH      = 28;   // inside the cell border (was 24)
constexpr int kIconYOffs  = 1;    // 1 px below top border

// Name strip below the grid.
constexpr int kNameStripY = 102;
constexpr int kNameStripH = 10;   // ~9 px font line + padding

constexpr int kMaxItems = 30;     // enough for v5 inventories; trims past this

struct Entry {
	int  obj_id;
	char name[24];
	// Phase 2 hook: when non-null, a kIconW×kIconH RGB565 thumbnail
	// painted into the cell instead of the truncated-name fallback.
	const uint16_t *icon;
};

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
		const byte *raw = eng->publicGetObjOrActorName(obj);
		if (!raw || !raw[0]) continue;
		// SCUMM v5 OBCD names can begin with 0xFF control sequences
		// (object/variable expansion).  Pieces of eight (object 478)
		// in MI1 is the canonical case — its name starts with
		// 0xFF 0x04 NN NN (VAR_GOLD expansion) followed by " pieces
		// of eight".  Run convertMessageToString first so the
		// expansion happens, then strip any leftover non-printable
		// bytes / '@' padding.  Without the expansion the strip
		// loop below sees 0xFF first byte → drops the whole entry,
		// hiding gold from the inventory entirely.
		byte expanded[64];
		expanded[0] = 0;
		eng->publicConvertMessageToString(raw, expanded, sizeof(expanded));
		const byte *name = expanded;
		if (!name[0]) continue;
		out[n].obj_id = obj;
		out[n].icon   = nullptr;
		int dst = 0;
		for (int j = 0; j < (int)sizeof(out[n].name) - 1 && name[j]; ++j) {
			const byte b = name[j];
			if (b == '@')  break;          // SCUMM end-of-string padding
			if (b < 32 || b > 126) break;  // non-printable / leftover markup
			out[n].name[dst++] = (char)b;
		}
		while (dst > 0 && out[n].name[dst - 1] == ' ') --dst;
		out[n].name[dst] = 0;
		if (dst == 0) continue;            // nothing renderable
		++n;
	}
	return n;
}

// Icon cache pool.  Sized to kMaxCachedIcons rather than kMaxItems
// because the engine's verb-script only keeps rtVerb resources for the
// currently-visible inventory page (verbids 101..106 → 6 slots) — items
// beyond that page have no decodable image right now, so we'd never
// fill more than 6 cache slots anyway.  Heap-allocated at run() entry
// and freed at exit: ~11 KB lives only while the picker is open, vs
// permanently in BSS.
constexpr int kMaxCachedIcons = 6;
constexpr int kIconBufWords    = kIconW * kIconH;  // RGB565 px per slot
// CLUT8 scratch the decoder writes strip data into, before the
// palette/blend pass downsamples it to RGB565.  Sized to worst-case
// Indy4 inventory OBIM (80 wide × 32 tall = 2560 bytes).  Allocated on
// the same heap block as the icon cache.
constexpr int kScratchW = 80;
constexpr int kScratchH = 32;
constexpr int kScratchBytes = kScratchW * kScratchH;

// Populate Entry::icon for each item that has a current inventory verb
// slot in the engine.  `cache` is a kMaxCachedIcons × kIconBufWords
// uint16_t pool the caller owns.  Returns the number of entries that
// ended up with a cached icon — items beyond the engine's current
// panel page stay text-in-cell.
int captureItemIcons(ScummEngine *eng, OSystem_Thumby *osys,
                     Entry *entries, int count,
                     uint16_t *cache, uint8_t *scratch) {
	if (!eng || !osys || !cache || !scratch) return 0;
	int n = 0;
	for (int i = 0; i < count; ++i) {
		entries[i].icon = nullptr;
		if (n >= kMaxCachedIcons) continue;
		const int verb_slot = eng->publicFindInventoryVerbSlot(entries[i].obj_id);
		if (verb_slot <= 0) continue;
		uint16_t *slot = cache + n * kIconBufWords;
		if (osys->captureVerbIcon(eng, verb_slot,
		                          slot, kIconW, kIconH,
		                          scratch, kScratchW,
		                          kScratchW, kScratchH)) {
			entries[i].icon = slot;
			++n;
		}
	}
	return n;
}

void draw_cell_border(int cx, int cy, uint16_t color) {
	for (int x = 0; x < kCellW; ++x) {
		tsb::platform::lcd_pixel(cx + x,             cy,             color);
		tsb::platform::lcd_pixel(cx + x,             cy + kCellH - 1, color);
	}
	for (int y = 0; y < kCellH; ++y) {
		tsb::platform::lcd_pixel(cx,             cy + y, color);
		tsb::platform::lcd_pixel(cx + kCellW - 1, cy + y, color);
	}
}

void blit_icon(const uint16_t *src, int cx, int cy) {
	// Centre the icon inside the cell content area (40×24, 1 px below
	// the top border).
	const int ox = cx + 1;
	const int oy = cy + 1 + kIconYOffs;
	for (int y = 0; y < kIconH; ++y) {
		const uint16_t *row = src + y * kIconW;
		for (int x = 0; x < kIconW; ++x) {
			tsb::platform::lcd_pixel(ox + x, oy + y, row[x]);
		}
	}
}

void draw_cell_text(int cx, int cy, const char *name, uint16_t color) {
	// Truncate the name to fit a single line inside the cell.  The
	// bottom strip carries the full (marquee'd) name; the in-cell text
	// is just a recognisable hint.
	char buf[10];
	int len = 0;
	for (; len < (int)sizeof(buf) - 1 && name[len]; ++len) buf[len] = name[len];
	buf[len] = '\0';
	// Trim until it fits the 38 px usable width.
	while (len > 0 && tsb::mi_font::text_width(buf) > kCellW - 4) {
		buf[--len] = '\0';
	}
	const int tw = tsb::mi_font::text_width(buf);
	const int tx = cx + (kCellW - tw) / 2;
	// Centre vertically — MI font glyph height ≈ 7 px.
	const int ty = cy + (kCellH - 7) / 2;
	tsb::mi_font::draw(tx, ty, buf, color);
}

void paint_grid(OSystem_Thumby *osys, const Entry *entries, int count, int sel) {
	if (osys) osys->renderSnapshotToFramebuffer();

	// Dim only the area the grid + strips actually cover (preserves the
	// sentence strip at the bottom which present() owns).
	tsb::platform::lcd_dim_box(0, 0, 128, 120);

	// Header line.
	char hdr[24];
	std::snprintf(hdr, sizeof(hdr), "INVENTORY");
	tsb::mi_font::draw(2, 0, hdr, kHilite);
	if (count > 0) {
		char pos[12];
		std::snprintf(pos, sizeof(pos), "%d/%d", sel + 1, count);
		const int pw = tsb::mi_font::text_width(pos);
		tsb::mi_font::draw(128 - pw - 2, 0, pos, kWhite);
	}

	if (count == 0) {
		tsb::mi_font::draw(48, 60, "(empty)", kDim);
		tsb::platform::lcd_present_now();
		return;
	}

	// Determine top visible row so the selection stays in view.
	const int sel_row = sel / kGridCols;
	const int max_row = (count - 1) / kGridCols;
	int top_row = sel_row - (kGridRows - 1);
	if (top_row < 0) top_row = 0;
	int max_top = max_row - (kGridRows - 1);
	if (max_top < 0) max_top = 0;
	if (top_row > max_top) top_row = max_top;
	// Bias upward when there's room: prefer the selection sitting in
	// the centre row if possible (less disorienting).
	if (sel_row < top_row + 1 && top_row > 0) --top_row;

	for (int row = 0; row < kGridRows; ++row) {
		for (int col = 0; col < kGridCols; ++col) {
			const int idx = (top_row + row) * kGridCols + col;
			if (idx >= count) continue;
			const int cx = kGridX + col * kCellW;
			const int cy = kGridY + row * kCellH;
			const bool is_sel = (idx == sel);
			draw_cell_border(cx, cy, is_sel ? kHilite : kDim);
			if (entries[idx].icon) {
				blit_icon(entries[idx].icon, cx, cy);
			} else {
				draw_cell_text(cx, cy,
				               entries[idx].name,
				               is_sel ? kHilite : kWhite);
			}
		}
	}

	// Bottom marquee strip — full selected name.
	static uint32_t s_marquee_frame = 0; ++s_marquee_frame;
	const char *name = entries[sel].name;
	const int   nw   = tsb::mi_font::text_width(name);
	constexpr int kStripPad  = 2;
	constexpr int kStripMaxW = 128 - 2 * kStripPad;
	const int scroll = tsb::mi_font::marquee_offset(nw, kStripMaxW,
	                                                s_marquee_frame);
	tsb::mi_font::draw_clipped(kStripPad - scroll, kNameStripY,
	                           name, kHilite,
	                           kStripPad, kStripPad + kStripMaxW);

	tsb::platform::lcd_present_now();
}

// Legacy text-list paint — used when no items have cached icons.  This
// is the pre-grid renderer kept verbatim apart from constants pulled in
// from the new globals; we keep it because text-only games (MI1, MI2,
// Indy3 — no inventory icons in the original panels) read more cleanly
// as a list than as 3×3 grid of textboxes.
constexpr int kListBoxX = 0;
constexpr int kListBoxY = 60;
constexpr int kListBoxW = 128;
constexpr int kListBoxH = 60;

void paint_list(OSystem_Thumby *osys, const Entry *entries, int count, int sel) {
	if (osys) osys->renderSnapshotToFramebuffer();

	tsb::platform::lcd_dim_box(kListBoxX, kListBoxY, kListBoxW, kListBoxH);
	for (int x = 0; x < kListBoxW; x++) {
		tsb::platform::lcd_pixel(kListBoxX + x, kListBoxY,             kDim);
		tsb::platform::lcd_pixel(kListBoxX + x, kListBoxY + kListBoxH - 1, kDim);
	}
	for (int y = 0; y < kListBoxH; y++) {
		tsb::platform::lcd_pixel(kListBoxX,             kListBoxY + y, kDim);
		tsb::platform::lcd_pixel(kListBoxX + kListBoxW - 1, kListBoxY + y, kDim);
	}

	tsb::mi_font::draw(kListBoxX + 4, kListBoxY + 3, "INVENTORY", kHilite);

	if (count == 0) {
		tsb::mi_font::draw(kListBoxX + 8, kListBoxY + 26, "(empty)", kDim);
		tsb::platform::lcd_present_now();
		return;
	}

	const int max_visible = 6;
	int top = sel - max_visible / 2;
	if (top < 0) top = 0;
	if (top + max_visible > count) top = count - max_visible;
	if (top < 0) top = 0;

	constexpr int kRowX    = kListBoxX + 2;
	constexpr int kRowMaxW = kListBoxW - 4;
	static uint32_t s_marquee_frame = 0; ++s_marquee_frame;

	for (int i = 0; i < max_visible && top + i < count; i++) {
		const int idx = top + i;
		const int y = kListBoxY + 14 + i * 7;
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

// Confirm sel-th inventory item.  Returns true if the engine took over
// the dispatch (verb-script will run); false to fall back to the
// verb-slot scan.
bool dispatch_item(ScummEngine *engine, int obj) {
	if (obj <= 0) return false;
	if (engine->publicDispatchInventoryClick(obj)) return true;

	// Fallback for unwired games — same logic as before, kept as a
	// safety net for any v5 game whose per-game var lookup we haven't
	// added to publicDispatchInventoryClick yet.  See verbs.cpp
	// kImageVerbType + the comment in publicDispatchInventoryClick for
	// the caveats (works only when the engine panel hasn't scrolled).
	int idx = 0, target_verbid = 0;
	for (int v = 1; v < engine->numVerbs(); ++v) {
		const VerbSlot &vs = engine->_verbs[v];
		if (vs.saveid || !vs.curmode || !vs.verbid) continue;
		if (vs.verbid < 101 || vs.verbid > 106) continue;
		if (idx == 0) { target_verbid = vs.verbid; break; }
		++idx;
	}
	if (target_verbid > 0) {
		engine->publicRunInputScript(kVerbClickArea,
		                             target_verbid, 1);
	}
	return false;
}

}  // anonymous

void run(ScummEngine *engine) {
	if (!engine) return;
	OSystem_Thumby *osys = (::g_system != nullptr)
		? static_cast<OSystem_Thumby *>(::g_system) : nullptr;

	static Entry entries[kMaxItems];
	int count = gather_inventory(engine, entries, kMaxItems);

	// Heap-alloc the icon cache + decode scratch in one block so their
	// ~13 KB combined lives only while the picker is open.  Cache
	// holds RGB565 thumbnails (referenced by Entry::icon); scratch is
	// the temporary CLUT8 buffer the engine decodes each icon into
	// before the palette/blend pass.  RAII guard frees both on any
	// run() exit path.
	constexpr size_t kCacheBytes = sizeof(uint16_t) * kMaxCachedIcons * kIconBufWords;
	uint8_t *block = (uint8_t *)malloc(kCacheBytes + kScratchBytes);
	struct CacheGuard { uint8_t *p; ~CacheGuard() { free(p); } } guard{block};
	uint16_t *icon_cache = block ? (uint16_t *)block : nullptr;
	uint8_t  *scratch    = block ? block + kCacheBytes : nullptr;
	const int icon_count = (icon_cache && scratch)
	    ? captureItemIcons(engine, osys, entries, count, icon_cache, scratch)
	    : 0;
	const bool use_grid = (icon_count > 0);

	int sel = 0;
	bool prev_up = false, prev_down = false;
	bool prev_left = false, prev_right = false;

	while (true) {
		if (use_grid) paint_grid(osys, entries, count, sel);
		else          paint_list(osys, entries, count, sel);

		tsb::platform::Input in{};
		if (!tsb::platform::poll_input(&in)) return;
		if (in.menu_pressed || in.b_pressed || in.rb_pressed) return;

		if (count == 0) { tsb::platform::sleep_ms(16); continue; }

		const bool up_edge    = in.dpad_up    && !prev_up;
		const bool down_edge  = in.dpad_down  && !prev_down;
		const bool left_edge  = in.dpad_left  && !prev_left;
		const bool right_edge = in.dpad_right && !prev_right;
		prev_up    = in.dpad_up;
		prev_down  = in.dpad_down;
		prev_left  = in.dpad_left;
		prev_right = in.dpad_right;

		if (use_grid) {
			// Grid navigation — no wrap; clamp at edges.
			const int row = sel / kGridCols;
			const int col = sel % kGridCols;
			const int max_row = (count - 1) / kGridCols;
			if (up_edge    && row > 0)        sel -= kGridCols;
			if (down_edge  && row < max_row) {
				const int next = sel + kGridCols;
				if (next < count) sel = next;
			}
			if (left_edge  && col > 0)        sel -= 1;
			if (right_edge && col < kGridCols - 1 && sel + 1 < count) sel += 1;
		} else {
			// List navigation — wrap (matches previous behaviour).
			if (up_edge)   sel = (sel + count - 1) % count;
			if (down_edge) sel = (sel + 1) % count;
		}

		if (in.a_pressed) {
			dispatch_item(engine, entries[sel].obj_id);
			return;
		}
		tsb::platform::sleep_ms(16);
	}
}

}  // namespace inventory_picker
}  // namespace tsb
