// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — RP2350 device save backend.
//
// Single save slot at the top 64 KB of flash (16 sectors × 4 KB).  Reads
// come straight from XIP — no copy.  Writes accumulate in a 256-byte page
// buffer; a full page programs into flash, and crossing into a new sector
// erases that sector first.
//
// Flash ops disable interrupts and run from RAM (XIP is suspended).  Audio
// PWM IRQ is paused for the duration; the save UI shows "Saving…" so the
// silence is visible.  Single-core build — no multicore lockout needed.

#include "save_backend.h"
#include "common/stream.h"
#include "platform.h"

#include "hardware/flash.h"
#include "hardware/sync.h"

#include <cstring>

namespace tsb {
namespace save_backend {

namespace {

// Reserved region — top 64 KB of 16 MB flash.  Far above the 4.7 MB
// firmware/data footprint; will not collide with future growth unless we
// blow another 11 MB of flash, which won't happen.
constexpr uint32_t kFlashRegionOffset = 16u * 1024u * 1024u - 64u * 1024u;  // 0x00FF0000
constexpr uint32_t kFlashRegionSize   = 64u * 1024u;
constexpr uintptr_t kFlashXipBase     = 0x10000000u;

constexpr uint32_t kHeaderMagic = 0x53425354u;  // 'TSBS' little-endian
constexpr uint16_t kHeaderVersion = 1;

struct __attribute__((packed)) Header {
	uint32_t magic;
	uint16_t version;
	uint16_t reserved;
	uint32_t payload_size;
	uint32_t reserved2;
};
static_assert(sizeof(Header) == 16, "save header must be 16 bytes");

const uint8_t *xip_ptr(uint32_t flash_offs) {
	return reinterpret_cast<const uint8_t *>(kFlashXipBase + flash_offs);
}

const Header *header_ptr() {
	return reinterpret_cast<const Header *>(xip_ptr(kFlashRegionOffset));
}

// FlashWriteStream — page-buffered, sector-erasing.
class FlashWriteStream : public Common::SeekableWriteStream {
public:
	FlashWriteStream() : _pos(0), _err(false), _finalized(false) {
		std::memset(_page, 0xFF, FLASH_PAGE_SIZE);
		// Erase the entire reserved region up front.  ~50 ms × 16 = ~800 ms
		// total; UI shows "Saving…" so the latency is fine, and it lets
		// the rest of the save be a simple sequential program.
		uint32_t flags = save_and_disable_interrupts();
		flash_range_erase(kFlashRegionOffset, kFlashRegionSize);
		restore_interrupts(flags);

		// Reserve room for the header at the start.  Save payload begins
		// at offset sizeof(Header) (16 bytes).  We seek past it; finalize
		// writes the header retroactively.
		_pos = sizeof(Header);
	}

	~FlashWriteStream() override {
		if (!_finalized) finalize();
	}

	uint32 write(const void *dataPtr, uint32 dataSize) override {
		if (_err || _finalized) return 0;
		const uint8_t *src = static_cast<const uint8_t *>(dataPtr);
		uint32 left = dataSize;
		while (left > 0) {
			uint32 page_idx = _pos / FLASH_PAGE_SIZE;
			uint32 in_page  = _pos % FLASH_PAGE_SIZE;
			uint32 chunk    = FLASH_PAGE_SIZE - in_page;
			if (chunk > left) chunk = left;

			std::memcpy(_page + in_page, src, chunk);
			src   += chunk;
			_pos  += chunk;
			left  -= chunk;

			// Page complete — flush.
			if ((_pos % FLASH_PAGE_SIZE) == 0) {
				program_page(page_idx);
				std::memset(_page, 0xFF, FLASH_PAGE_SIZE);
			}
		}
		return dataSize;
	}

	bool flush() override { return !_err; }

	void finalize() override {
		if (_finalized) return;
		_finalized = true;

		// Flush the partial page (with payload data, if any).
		uint32 page_idx = _pos / FLASH_PAGE_SIZE;
		uint32 in_page  = _pos % FLASH_PAGE_SIZE;
		if (in_page > 0) {
			program_page(page_idx);
		}

		// Write the header into the very first page.  We held off until
		// finalize so payload_size is known.
		Header hdr{};
		hdr.magic = kHeaderMagic;
		hdr.version = kHeaderVersion;
		hdr.reserved = 0;
		hdr.payload_size = (uint32_t)(_pos - sizeof(Header));
		hdr.reserved2 = 0;

		std::memset(_page, 0xFF, FLASH_PAGE_SIZE);
		std::memcpy(_page, &hdr, sizeof(hdr));
		program_page(0);
	}

	int64 pos() const override { return (int64)_pos; }

	bool seek(int64 offset, int whence) override {
		// Stream-based saver doesn't actually call seek — saveload.cpp
		// writes sequentially.  But saveSaveGameHeader does an out->finalize()
		// at the very end.  Keep this minimal.
		_err = true;
		return false;
	}

	int64 size() const override { return (int64)_pos; }

	bool err() const override { return _err; }
	void clearErr() override { _err = false; }

private:
	// Program one page (256 B) into the reserved flash region.  Sector
	// erase already happened in the ctor — we only program here.
	__attribute__((noinline))
	void program_page(uint32 page_idx) {
		uint32_t flash_offs = kFlashRegionOffset + page_idx * FLASH_PAGE_SIZE;
		if (flash_offs + FLASH_PAGE_SIZE > kFlashRegionOffset + kFlashRegionSize) {
			_err = true;
			tsb::platform::log("[save] flash overflow: page %u", page_idx);
			return;
		}
		uint32_t flags = save_and_disable_interrupts();
		flash_range_program(flash_offs, _page, FLASH_PAGE_SIZE);
		restore_interrupts(flags);
	}

	uint8_t _page[FLASH_PAGE_SIZE];
	uint32  _pos;        // logical position from start of region (header + payload)
	bool    _err;
	bool    _finalized;
};

// FlashReadStream — direct XIP pointer arithmetic, no buffer.
class FlashReadStream : public Common::SeekableReadStream {
public:
	FlashReadStream(const uint8_t *base, uint32 size)
	    : _base(base), _size(size), _pos(0), _eos(false), _err(false) {}

	uint32 read(void *dataPtr, uint32 dataSize) override {
		if (_pos >= _size) { _eos = true; return 0; }
		uint32 left = _size - _pos;
		uint32 n = (dataSize < left) ? dataSize : left;
		std::memcpy(dataPtr, _base + _pos, n);
		_pos += n;
		if (n < dataSize) _eos = true;
		return n;
	}

	bool eos() const override { return _eos; }
	bool err() const override { return _err; }
	void clearErr() override { _eos = _err = false; }

	int64 pos() const override { return (int64)_pos; }

	bool seek(int64 offset, int whence) override {
		int64 newpos;
		switch (whence) {
			case SEEK_SET: newpos = offset; break;
			case SEEK_CUR: newpos = (int64)_pos + offset; break;
			case SEEK_END: newpos = (int64)_size + offset; break;
			default: return false;
		}
		if (newpos < 0 || newpos > (int64)_size) return false;
		_pos = (uint32)newpos;
		_eos = false;
		return true;
	}

	int64 size() const override { return (int64)_size; }

	const void *getRawPointer(uint32 sz) override {
		if (_pos + sz > _size) return nullptr;
		const void *p = _base + _pos;
		_pos += sz;
		return p;
	}

private:
	const uint8_t *_base;
	uint32 _size;
	uint32 _pos;
	bool _eos;
	bool _err;
};

}  // anonymous

Common::SeekableWriteStream *open_for_writing() {
	return new FlashWriteStream();
}

Common::SeekableReadStream *open_for_reading() {
	if (!has_save()) return nullptr;
	const Header *hdr = header_ptr();
	const uint8_t *payload = xip_ptr(kFlashRegionOffset) + sizeof(Header);
	return new FlashReadStream(payload, hdr->payload_size);
}

bool has_save() {
	const Header *hdr = header_ptr();
	if (hdr->magic != kHeaderMagic) return false;
	if (hdr->version != kHeaderVersion) return false;
	if (hdr->payload_size == 0 || hdr->payload_size > kFlashRegionSize - sizeof(Header)) return false;
	return true;
}

}  // namespace save_backend
}  // namespace tsb
