/* ThumbyScummby — 4bpp packed text overlay surface.
 *
 * Replaces Graphics::Surface for _textSurface on THUMBY_DEVICE, halving
 * the 64 KB CLUT8 storage to 32 KB by packing two palette indices per
 * byte. Used to fit v5 Atlantis (and other v5 games) within the
 * RP2350's heap budget.
 *
 * Encoding (4 bits per pixel):
 *   nibble 0x0..0x0E  -> palette indices 0..14
 *   nibble 0x0F       -> transparent (== CHARSET_MASK_TRANSPARENCY 0xFD
 *                        in the 8bpp world)
 *
 * Palette-index writes outside 0..14 are clamped to 0x0E and the first
 * occurrence per session is logged so we discover empirically whether
 * any v5 game text actually uses indices in that range. CHARSET_MASK_-
 * TRANSPARENCY (0xFD) is the one well-known sentinel and gets mapped to
 * 0x0F on write / mapped back to 0xFD on read.
 *
 * API rationale: scummvm's text-rendering code writes pixels through
 * raw pointer arithmetic via Graphics::Surface::getBasePtr() + pitch.
 * That access pattern can't be silently redirected at packed storage
 * (a byte write would corrupt two adjacent pixels). So we expose an
 * explicit-call API and convert each call site:
 *
 *   setPixel(x, y, v)         -> one nibble
 *   fillSpan(y, x0, len, v)   -> efficient horizontal run
 *   fillRect(r, v)            -> rectangle fill
 *   clearTransparent()        -> fill all with 0xF (== 0xFD for the
 *                                composite path)
 *   readRow(dst, y, x0, len)  -> unpack into caller buffer (for the
 *                                compositor in drawStripToScreen)
 *
 * For matters of API parity with Graphics::Surface (so call sites can
 * still query w/h/pitch and bytesPerPixel without thinking too hard),
 * we expose w/h directly and pitch as the logical bytes-per-row (= w),
 * matching what 8bpp callers expect. The actual storage row stride
 * is w/2 and is internal.
 */

#ifndef SCUMM_PACKED_TEXT_SURFACE_H
#define SCUMM_PACKED_TEXT_SURFACE_H

#include "common/scummsys.h"
#include "common/rect.h"

namespace Scumm {

// Sentinel matching CHARSET_MASK_TRANSPARENCY at the 8bpp boundary.
// Kept in sync with scumm/gfx.h #define.
static constexpr byte kTextTransparent8 = 0xFD;
static constexpr byte kTextTransparent4 = 0x0F;
static constexpr byte kTextMaxPalette4  = 0x0E;  // clamp ceiling for valid indices

class PackedTextSurface {
public:
	// Parity fields with Graphics::Surface for cheap callsite reads.
	int16  w = 0;
	int16  h = 0;
	int32  pitch = 0;  // logical bytes per row (= w), NOT storage row stride.

	PackedTextSurface() = default;
	~PackedTextSurface() { free(); }

	// Allocate storage for w_*h_ pixels at 4bpp.  pitch is set to w_.
	void create(int16 w_, int16 h_);

	// Release storage.  Safe to call on already-freed surface.
	void free();

	// Pixel access.  encode/decode are inline-friendly for the hot
	// glyph-writing path.
	byte getPixel(int x, int y) const;
	void setPixel(int x, int y, byte v8);

	// Horizontal-run fill.  Far cheaper than per-pixel for the
	// "clear a glyph row" / "fillRect a backColor strip" patterns.
	void fillSpan(int y, int x0, int len, byte v8);

	// Rect fill.  v8 is the 8bpp palette index (or transparent
	// sentinel) — the function maps to 4bpp internally.
	void fillRect(const Common::Rect &r, byte v8);

	// Whole-surface clear to transparent (== 0xFD in 8bpp).  Used by
	// the engine's clearTextSurface() / banner reset.
	void clearTransparent();

	// Whole-surface fill to an arbitrary 8bpp value.  Used by Towns /
	// scriptv4 fillRect-with-zero patterns.
	void fillAll(byte v8);

	// Bulk read into a caller-supplied 8bpp scratch buffer.  Used by
	// drawStripToScreen's composite read path which traverses one
	// strip of text-surface pixels per output row.  Returns kText-
	// Transparent8 (0xFD) where the stored nibble is 0xF.
	void readRow(byte *dst, int y, int x0, int len) const;

	// Test if the entire span at (y, x0..x0+len) is transparent.
	// Used by composite to take the fast (room-pixel only) branch.
	bool spanIsAllTransparent(int y, int x0, int len) const;

	// Diagnostic: peek at storage size in bytes (useful for heap
	// accounting in PH dumps).
	int storageSizeBytes() const { return _bytes_per_row * h; }

private:
	byte *_storage = nullptr;
	int16 _bytes_per_row = 0;   // = (w + 1) / 2

	// First-out-of-range warning latch — print once if any callsite
	// tries to write a palette index in 0x0F..0xFC range.
	static bool _warnedOutOfRange;

	static byte encode(byte v8);   // 8bpp -> nibble, handles sentinel + clamp
	static byte decode(byte n4);   // nibble -> 8bpp, expands sentinel
};

}  // namespace Scumm

#endif  // SCUMM_PACKED_TEXT_SURFACE_H
