/* ThumbyScummby — 4bpp packed text overlay surface implementation. */

#include "scumm/packed_text_surface.h"

#include <cstdlib>
#include <cstring>

namespace Scumm {

bool PackedTextSurface::_warnedOutOfRange = false;

void PackedTextSurface::create(int16 w_, int16 h_) {
	free();
	w = w_;
	h = h_;
	pitch = w_;                        // logical bytes per row (8bpp view)
	_bytes_per_row = (w_ + 1) / 2;     // packed storage row stride
	const size_t bytes = (size_t)_bytes_per_row * (size_t)h_;
	_storage = (byte *)std::malloc(bytes);
	if (_storage) {
		// Initialise to transparent (0xFF = two 0xF nibbles).
		std::memset(_storage, 0xFF, bytes);
	}
}

void PackedTextSurface::free() {
	if (_storage) {
		std::free(_storage);
		_storage = nullptr;
	}
	w = h = 0;
	pitch = 0;
	_bytes_per_row = 0;
}

// 8bpp -> nibble.  Special-cases the transparent sentinel; clamps
// out-of-range palette indices and warns once.
byte PackedTextSurface::encode(byte v8) {
	if (v8 == kTextTransparent8) return kTextTransparent4;
	if (v8 <= kTextMaxPalette4)  return v8;
	// Out-of-range palette index (15..252) — clamp to the highest
	// representable.  Tracked via the _warnedOutOfRange latch in case
	// we need to re-instrument; visually the colour will be wrong if
	// it fires.
	_warnedOutOfRange = true;
	return kTextMaxPalette4;
}

byte PackedTextSurface::decode(byte n4) {
	return (n4 == kTextTransparent4) ? kTextTransparent8 : n4;
}

byte PackedTextSurface::getPixel(int x, int y) const {
	if (!_storage || (unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h)
		return kTextTransparent8;
	const byte b = _storage[y * _bytes_per_row + (x >> 1)];
	const byte n = (x & 1) ? (b >> 4) : (b & 0x0F);
	return decode(n);
}

void PackedTextSurface::setPixel(int x, int y, byte v8) {
	if (!_storage || (unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h)
		return;
	const byte n = encode(v8);
	byte &b = _storage[y * _bytes_per_row + (x >> 1)];
	if (x & 1)
		b = (b & 0x0F) | (n << 4);
	else
		b = (b & 0xF0) | n;
}

void PackedTextSurface::fillSpan(int y, int x0, int len, byte v8) {
	if (!_storage || (unsigned)y >= (unsigned)h) return;
	if (x0 < 0) { len += x0; x0 = 0; }
	if (x0 + len > w) len = w - x0;
	if (len <= 0) return;
	const byte n = encode(v8);
	const byte packed = (byte)(n | (n << 4));  // both nibbles same
	byte *row = _storage + y * _bytes_per_row;
	int x = x0;
	int end = x0 + len;
	// Leading odd boundary
	if (x & 1) {
		row[x >> 1] = (row[x >> 1] & 0x0F) | (n << 4);
		x++;
	}
	// Aligned middle
	const int middle_end = end & ~1;
	if (x < middle_end) {
		std::memset(row + (x >> 1), packed, (middle_end - x) >> 1);
		x = middle_end;
	}
	// Trailing odd boundary
	if (x < end) {
		row[x >> 1] = (row[x >> 1] & 0xF0) | n;
	}
}

void PackedTextSurface::fillRect(const Common::Rect &r, byte v8) {
	int y0 = r.top, y1 = r.bottom;
	int x0 = r.left, x1 = r.right;
	if (y0 < 0) y0 = 0;
	if (y1 > h) y1 = h;
	if (x0 < 0) x0 = 0;
	if (x1 > w) x1 = w;
	for (int y = y0; y < y1; y++) {
		fillSpan(y, x0, x1 - x0, v8);
	}
}

void PackedTextSurface::clearTransparent() {
	if (!_storage) return;
	std::memset(_storage, 0xFF, (size_t)_bytes_per_row * (size_t)h);
}

void PackedTextSurface::fillAll(byte v8) {
	if (!_storage) return;
	const byte n = encode(v8);
	const byte packed = (byte)(n | (n << 4));
	std::memset(_storage, packed, (size_t)_bytes_per_row * (size_t)h);
}

void PackedTextSurface::readRow(byte *dst, int y, int x0, int len) const {
	if (!_storage || (unsigned)y >= (unsigned)h) {
		std::memset(dst, kTextTransparent8, len);
		return;
	}
	const byte *row = _storage + y * _bytes_per_row;
	int x = x0;
	int written = 0;
	while (written < len && x < w) {
		const byte b = row[x >> 1];
		const byte n = (x & 1) ? (b >> 4) : (b & 0x0F);
		dst[written++] = decode(n);
		x++;
	}
	while (written < len) dst[written++] = kTextTransparent8;
}

bool PackedTextSurface::spanIsAllTransparent(int y, int x0, int len) const {
	if (!_storage || (unsigned)y >= (unsigned)h) return true;
	const byte *row = _storage + y * _bytes_per_row;
	int x = x0;
	int end = x0 + len;
	// Leading odd boundary
	if (x & 1 && x < end) {
		if (((row[x >> 1] >> 4) & 0x0F) != kTextTransparent4) return false;
		x++;
	}
	const int middle_end = end & ~1;
	// Aligned bytes — both nibbles must be 0xF, so byte == 0xFF.
	while (x < middle_end) {
		if (row[x >> 1] != 0xFF) return false;
		x += 2;
	}
	if (x < end) {
		if ((row[x >> 1] & 0x0F) != kTextTransparent4) return false;
	}
	return true;
}

}  // namespace Scumm
