// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — RP2350 device save backend.  Four slots at the top
// 192 KB of flash; each slot owns a 48 KB region (12 × 4 KB sectors).
//
// Region layout (per slot):
//   [0..15]       Header { magic, version, payload_size, reserved }
//   [16..5135]    Thumbnail (kThumbW × kThumbH × 2 bytes, RGB565)
//   [5136..5183]  Hint string (NUL-terminated, kHintBytes)
//   [5184..]      SCUMM save payload (whatever engine saveState writes)
//
// Writes accumulate in a 256-byte page buffer; full pages program into
// flash, sector boundaries erase first.  Flash ops disable interrupts
// and run from RAM (XIP suspended).  Reads come straight from XIP.

#include "save_backend.h"
#include "common/stream.h"
#include "platform.h"

#include "hardware/flash.h"
#include "hardware/sync.h"

#include <cstring>

namespace tsb {
namespace save_backend {

namespace {

// Reserved region — top 384 KB of 16 MB flash.  Slot N starts at
// kFlashRegionBase + N * kSlotRegionSize.  Well above the ~4.7 MB
// firmware/data footprint.  96 KB per slot fits Indy4's ~80 KB save
// payloads with headroom for v6 (DOTT) too.
constexpr uint32_t kFlashRegionBase  = 16u * 1024u * 1024u - 384u * 1024u;  // 0x00FA0000
constexpr uint32_t kSlotRegionSize   = 96u * 1024u;                          // 0x18000
constexpr uintptr_t kFlashXipBase    = 0x10000000u;

constexpr uint32_t kHeaderMagic   = 0x53425354u;  // 'TSBS' little-endian
constexpr uint16_t kHeaderVersion = 2;            // bumped: v1 was single-slot

struct __attribute__((packed)) Header {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t payload_size;   // total bytes after this header (thumb + hint + scumm)
    uint32_t reserved2;
};
static_assert(sizeof(Header) == 16, "save header must be 16 bytes");

constexpr uint32_t kMetaOffset    = sizeof(Header);                  // 16
constexpr uint32_t kHintOffset    = kMetaOffset + kThumbBytes;       // 5136
constexpr uint32_t kPayloadStart  = kHintOffset + kHintBytes;        // 5184
constexpr uint32_t kMaxPayload    = kSlotRegionSize - kPayloadStart; // ~43 KB

uint32_t slot_flash_offs(int slot) {
    return kFlashRegionBase + slot * kSlotRegionSize;
}

const uint8_t *xip_ptr(uint32_t flash_offs) {
    return reinterpret_cast<const uint8_t *>(kFlashXipBase + flash_offs);
}

const Header *slot_header(int slot) {
    return reinterpret_cast<const Header *>(xip_ptr(slot_flash_offs(slot)));
}

bool slot_header_valid(const Header *hdr) {
    if (hdr->magic != kHeaderMagic) return false;
    if (hdr->version != kHeaderVersion) return false;
    if (hdr->payload_size < kThumbBytes + kHintBytes) return false;
    if (hdr->payload_size > kSlotRegionSize - sizeof(Header)) return false;
    return true;
}

// FlashWriteStream — sequential page-buffered writer scoped to one slot.
class FlashWriteStream : public Common::SeekableWriteStream {
public:
    FlashWriteStream(int slot, const uint16_t *thumb, const char *hint)
        : _slot(slot), _pos(0), _err(false), _finalized(false) {
        std::memset(_page, 0xFF, FLASH_PAGE_SIZE);

        // Erase the entire slot region up front.  ~50 ms × 12 sectors =
        // ~600 ms; UI shows "Saving…" so the latency is visible.
        const uint32_t base = slot_flash_offs(_slot);
        uint32_t flags = save_and_disable_interrupts();
        flash_range_erase(base, kSlotRegionSize);
        restore_interrupts(flags);

        // Reserve the header slot — it gets filled at finalize() once
        // payload_size is known.  Write thumbnail + hint immediately.
        _pos = sizeof(Header);

        if (thumb) {
            write_raw(reinterpret_cast<const uint8_t *>(thumb), kThumbBytes);
        } else {
            uint8_t zero = 0;
            for (uint32_t i = 0; i < kThumbBytes; ++i) write_raw(&zero, 1);
        }

        // Hint string padded to kHintBytes with NULs.
        char hintbuf[kHintBytes];
        std::memset(hintbuf, 0, sizeof(hintbuf));
        if (hint) {
            std::strncpy(hintbuf, hint, sizeof(hintbuf) - 1);
        }
        write_raw(reinterpret_cast<const uint8_t *>(hintbuf), kHintBytes);
        // _pos now sits at kPayloadStart — engine's writes land in payload.
    }

    ~FlashWriteStream() override {
        if (!_finalized) finalize();
    }

    uint32 write(const void *dataPtr, uint32 dataSize) override {
        if (_err || _finalized) return 0;
        return write_raw(static_cast<const uint8_t *>(dataPtr), dataSize);
    }

    bool flush() override { return !_err; }

    void finalize() override {
        if (_finalized) return;
        _finalized = true;

        // Flush partial page first (covers everything after the last
        // page-aligned byte we programmed).
        uint32 in_page = _pos % FLASH_PAGE_SIZE;
        if (in_page > 0) {
            uint32 page_idx = _pos / FLASH_PAGE_SIZE;
            program_page(page_idx);
        }

        // Write the header into page 0.  payload_size is _pos minus the
        // 16-byte header slot we reserved up front.
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

    int64 pos() const override { return (int64)_pos - (int64)kPayloadStart; }

    bool seek(int64 offset, int whence) override {
        // The engine never seeks during save; this is just a safety
        // marker for any future caller that tries.
        _err = true;
        return false;
    }

    int64 size() const override { return (int64)_pos - (int64)kPayloadStart; }

    bool err() const override { return _err; }
    void clearErr() override { _err = false; }

private:
    uint32 write_raw(const uint8_t *src, uint32 dataSize) {
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

            if ((_pos % FLASH_PAGE_SIZE) == 0) {
                program_page(page_idx);
                std::memset(_page, 0xFF, FLASH_PAGE_SIZE);
            }
        }
        return dataSize;
    }

    __attribute__((noinline))
    void program_page(uint32 page_idx) {
        const uint32_t base = slot_flash_offs(_slot);
        uint32_t flash_offs = base + page_idx * FLASH_PAGE_SIZE;
        if (flash_offs + FLASH_PAGE_SIZE > base + kSlotRegionSize) {
            _err = true;
            tsb::platform::log("[save] slot %d overflow page %u\n",
                               _slot, page_idx);
            return;
        }
        uint32_t flags = save_and_disable_interrupts();
        flash_range_program(flash_offs, _page, FLASH_PAGE_SIZE);
        restore_interrupts(flags);
    }

    uint8_t _page[FLASH_PAGE_SIZE];
    int     _slot;
    uint32  _pos;     // bytes written into region from start of region
    bool    _err;
    bool    _finalized;
};

// FlashReadStream — direct XIP pointer arithmetic, scoped to payload.
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

Common::SeekableWriteStream *open_for_writing(int slot,
                                              const uint16_t *thumb,
                                              const char *hint) {
    if (slot < 0 || slot >= kNumSlots) return nullptr;
    return new FlashWriteStream(slot, thumb, hint);
}

Common::SeekableReadStream *open_for_reading(int slot) {
    if (slot < 0 || slot >= kNumSlots) return nullptr;
    if (!has_save(slot)) return nullptr;
    const Header *hdr = slot_header(slot);
    const uint8_t *payload = xip_ptr(slot_flash_offs(slot)) + kPayloadStart;
    const uint32_t payload_size = hdr->payload_size - kThumbBytes - kHintBytes;
    return new FlashReadStream(payload, payload_size);
}

bool has_save(int slot) {
    if (slot < 0 || slot >= kNumSlots) return false;
    return slot_header_valid(slot_header(slot));
}

void enumerate_slots(SlotInfo *out) {
    if (!out) return;
    for (int s = 0; s < kNumSlots; ++s) {
        out[s].occupied = has_save(s);
        if (out[s].occupied) {
            const uint8_t *base = xip_ptr(slot_flash_offs(s));
            out[s].thumb = reinterpret_cast<const uint16_t *>(base + kMetaOffset);
            const char *hint = reinterpret_cast<const char *>(base + kHintOffset);
            std::memcpy(out[s].hint, hint, kHintBytes);
            out[s].hint[kHintBytes - 1] = '\0';  // safety NUL
        } else {
            out[s].thumb = nullptr;
            out[s].hint[0] = '\0';
        }
    }
}

}  // namespace save_backend
}  // namespace tsb
