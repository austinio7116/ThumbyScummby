// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — FatFs save backend (slot mode).
//
// One file per slot at /scumm/<game>/saves/slot<N>.sav.  File layout
// mirrors the flash backend so the on-wire format is shared:
//
//   [0..15]      Header  { magic, version, payload_size, reserved }
//   [16..]       Thumbnail (kThumbW × kThumbH RGB565)
//   [thumb_end]  Hint string (NUL-padded, kHintBytes)
//   [...]        SCUMM payload
//
// Writes are atomic: bytes go to slot<N>.sav.tmp, then f_rename to
// slot<N>.sav on finalize.  A power loss mid-write leaves the
// previous save intact.
//
// Thumbnails are *not* surfaced in this backend yet — caching all
// four (~18 KB) would blow the BSS budget, and streaming on every
// render would be too slow.  enumerate_slots returns thumb=nullptr;
// save_menu's blit_thumb degrades to a black box.  Reintroduce when
// the memory budget loosens.

#include "save_backend.h"
#include "common/stream.h"
#include "platform.h"
#include "game_table.h"

extern "C" {
#include "ff.h"
}

#include <cstdio>
#include <cstring>

namespace tsb {
namespace save_backend {

namespace {

constexpr uint32_t kHeaderMagic   = 0x53425354u;  // 'TSBS' little-endian
constexpr uint16_t kHeaderVersion = 3;            // matches flash backend

struct __attribute__((packed)) Header {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t payload_size;   // total bytes after header (thumb + hint + scumm)
    uint32_t reserved2;
};
static_assert(sizeof(Header) == 16, "save header must be 16 bytes");

constexpr uint32_t kMetaOffset   = sizeof(Header);
constexpr uint32_t kHintOffset   = kMetaOffset + kThumbBytes;
constexpr uint32_t kPayloadStart = kHintOffset + kHintBytes;

// Resolve /scumm/<subdir>/saves/slot<N>.sav.  Returns false if no
// game is currently selected (boot scan turned up nothing).
bool build_save_path(int slot, char *out, size_t outsz) {
    const char *sub = g_current_game ? g_current_game->subdir : nullptr;
    if (!sub || !*sub) return false;
    int n = std::snprintf(out, outsz, "/scumm/%s/saves/slot%d.sav",
                          sub, slot);
    return n > 0 && (size_t)n < outsz;
}

// Idempotently create /scumm/<subdir>/saves/.  Both parents may
// already exist; we don't care which f_mkdir calls return FR_EXIST.
void ensure_saves_dir() {
    const char *sub = g_current_game ? g_current_game->subdir : nullptr;
    if (!sub || !*sub) return;
    char path[40];
    std::snprintf(path, sizeof(path), "/scumm/%s/saves", sub);
    f_mkdir(path);
}

// Atomic-write stream.  Bytes flow into slot<N>.sav.tmp; finalize()
// patches the header and f_renames over slot<N>.sav.  Mid-write
// crashes leave the previous save intact (the rename is the commit).
class FatFsWriteStream : public Common::SeekableWriteStream {
public:
    FatFsWriteStream(int slot, const uint16_t *thumb, const char *hint)
        : _payload_bytes(0), _err(false), _finalized(false), _opened(false) {
        if (!build_save_path(slot, _final_path, sizeof(_final_path))) {
            _err = true; return;
        }
        ensure_saves_dir();
        int n = std::snprintf(_tmp_path, sizeof(_tmp_path), "%s.tmp",
                              _final_path);
        if (n <= 0 || (size_t)n >= sizeof(_tmp_path)) { _err = true; return; }

        f_unlink(_tmp_path);   // ignore: may not exist
        if (f_open(&_fp, _tmp_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
            _err = true; return;
        }
        _opened = true;

        // Reserve the header — overwritten in finalize() once
        // payload_size is known.
        uint8_t zeros[sizeof(Header)] = {0};
        UINT bw = 0;
        if (f_write(&_fp, zeros, sizeof(Header), &bw) != FR_OK ||
            bw != sizeof(Header)) { _err = true; return; }

        // Thumbnail.  Null thumb → all-zero block (same as flash
        // backend's "blanked thumbnail" path).
        if (thumb) {
            if (f_write(&_fp, thumb, kThumbBytes, &bw) != FR_OK ||
                bw != kThumbBytes) { _err = true; return; }
        } else {
            uint8_t zero[64] = {0};
            uint32_t left = kThumbBytes;
            while (left > 0) {
                uint32_t n2 = left < sizeof(zero) ? left : sizeof(zero);
                if (f_write(&_fp, zero, n2, &bw) != FR_OK || bw != n2) {
                    _err = true; return;
                }
                left -= n2;
            }
        }

        // Hint, NUL-padded.
        char hintbuf[kHintBytes];
        std::memset(hintbuf, 0, sizeof(hintbuf));
        if (hint) std::strncpy(hintbuf, hint, sizeof(hintbuf) - 1);
        if (f_write(&_fp, hintbuf, kHintBytes, &bw) != FR_OK ||
            bw != kHintBytes) { _err = true; return; }
    }

    ~FatFsWriteStream() override { if (!_finalized) finalize(); }

    uint32 write(const void *data, uint32 size) override {
        if (_err || _finalized) return 0;
        UINT bw = 0;
        if (f_write(&_fp, data, size, &bw) != FR_OK || bw != size) {
            _err = true; return 0;
        }
        _payload_bytes += size;
        return size;
    }

    bool flush() override {
        if (_err) return false;
        return f_sync(&_fp) == FR_OK;
    }

    void finalize() override {
        if (_finalized) return;
        _finalized = true;

        if (!_err && _opened) {
            Header hdr{};
            hdr.magic        = kHeaderMagic;
            hdr.version      = kHeaderVersion;
            hdr.payload_size = kThumbBytes + kHintBytes + _payload_bytes;

            if (f_lseek(&_fp, 0) != FR_OK) { _err = true; }
            else {
                UINT bw = 0;
                if (f_write(&_fp, &hdr, sizeof(hdr), &bw) != FR_OK ||
                    bw != sizeof(hdr)) _err = true;
            }
        }

        if (_opened) f_close(&_fp);

        if (!_err) {
            f_unlink(_final_path);                 // drop old slot (if any)
            if (f_rename(_tmp_path, _final_path) != FR_OK) {
                _err = true;
                tsb::platform::log("[save] rename fail slot=%s\n",
                                   _final_path);
            }
        } else {
            f_unlink(_tmp_path);                   // clean partial on error
        }
    }

    int64 pos()  const override { return (int64)_payload_bytes; }
    int64 size() const override { return (int64)_payload_bytes; }
    bool  err()  const override { return _err; }
    void  clearErr() override   { _err = false; }
    bool  seek(int64, int) override { _err = true; return false; }

private:
    FIL      _fp;
    uint32_t _payload_bytes;
    char     _final_path[48];
    char     _tmp_path[52];
    bool     _err;
    bool     _finalized;
    bool     _opened;
};

// Read stream.  Engine seeks freely during loadState; we forward to
// f_lseek with an offset bias of kPayloadStart so position 0 in the
// stream is byte 0 of the SCUMM payload.
class FatFsReadStream : public Common::SeekableReadStream {
public:
    FatFsReadStream(FIL fp, uint32_t payload_size)
        : _fp(fp), _payload_size(payload_size), _pos(0),
          _eos(false), _err(false) {}

    ~FatFsReadStream() override { f_close(&_fp); }

    uint32 read(void *data, uint32 size) override {
        if (_err) return 0;
        if (_pos >= _payload_size) { _eos = true; return 0; }
        uint32 left = _payload_size - _pos;
        if (size > left) size = left;
        UINT br = 0;
        if (f_read(&_fp, data, size, &br) != FR_OK) {
            _err = true; return 0;
        }
        _pos += br;
        if (br < size) _eos = true;
        return br;
    }

    bool eos() const override { return _eos; }
    bool err() const override { return _err; }
    void clearErr() override  { _eos = _err = false; }

    int64 pos()  const override { return (int64)_pos; }
    int64 size() const override { return (int64)_payload_size; }

    bool seek(int64 offset, int whence) override {
        int64 newpos;
        switch (whence) {
            case SEEK_SET: newpos = offset; break;
            case SEEK_CUR: newpos = (int64)_pos + offset; break;
            case SEEK_END: newpos = (int64)_payload_size + offset; break;
            default: return false;
        }
        if (newpos < 0 || newpos > (int64)_payload_size) return false;
        if (f_lseek(&_fp, kPayloadStart + (FSIZE_t)newpos) != FR_OK) {
            _err = true; return false;
        }
        _pos = (uint32)newpos;
        _eos = false;
        return true;
    }

private:
    FIL      _fp;
    uint32_t _payload_size;
    uint32_t _pos;
    bool     _eos;
    bool     _err;
};

// Read header + hint + thumbnail.  Caller passes a 4608-byte thumb
// buffer (or nullptr to skip thumb).  Returns true on a valid slot.
bool read_slot_meta(int slot, char *hint_out, uint32_t *payload_out,
                    uint16_t *thumb_out) {
    char path[48];
    if (!build_save_path(slot, path, sizeof(path))) return false;

    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK) return false;

    Header hdr;
    UINT br = 0;
    bool ok = (f_read(&fp, &hdr, sizeof(hdr), &br) == FR_OK)
           && br == sizeof(hdr)
           && hdr.magic   == kHeaderMagic
           && hdr.version == kHeaderVersion
           && hdr.payload_size >= kThumbBytes + kHintBytes;

    if (ok) {
        if (thumb_out) {
            // File cursor is already past the header — read thumb in
            // place, then continue to the hint.
            if (f_read(&fp, thumb_out, kThumbBytes, &br) != FR_OK ||
                br != kThumbBytes) {
                ok = false;
            }
        }
        if (ok) {
            // Seek explicitly to hint offset (covers both the thumb-
            // skipped and thumb-read cases).
            if (f_lseek(&fp, kHintOffset) != FR_OK ||
                f_read(&fp, hint_out, kHintBytes, &br) != FR_OK ||
                br != kHintBytes) {
                ok = false;
            }
            hint_out[kHintBytes - 1] = '\0';   // safety NUL
        }
        if (payload_out) {
            *payload_out = hdr.payload_size - kThumbBytes - kHintBytes;
        }
    }
    f_close(&fp);
    return ok;
}

}  // anonymous

Common::SeekableWriteStream *open_for_writing(int slot,
                                              const uint16_t *thumb,
                                              const char *hint) {
    if (slot < 0 || slot >= kNumSlots) return nullptr;
    return new FatFsWriteStream(slot, thumb, hint);
}

Common::SeekableReadStream *open_for_reading(int slot) {
    if (slot < 0 || slot >= kNumSlots) return nullptr;
    char path[48];
    if (!build_save_path(slot, path, sizeof(path))) return nullptr;

    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK) return nullptr;

    Header hdr;
    UINT br = 0;
    if (f_read(&fp, &hdr, sizeof(hdr), &br) != FR_OK ||
        br != sizeof(hdr) ||
        hdr.magic   != kHeaderMagic ||
        hdr.version != kHeaderVersion ||
        hdr.payload_size < kThumbBytes + kHintBytes) {
        f_close(&fp);
        return nullptr;
    }
    uint32_t payload_bytes = hdr.payload_size - kThumbBytes - kHintBytes;
    if (f_lseek(&fp, kPayloadStart) != FR_OK) {
        f_close(&fp);
        return nullptr;
    }
    return new FatFsReadStream(fp, payload_bytes);
}

bool has_save(int slot) {
    if (slot < 0 || slot >= kNumSlots) return false;
    char hint[kHintBytes];
    return read_slot_meta(slot, hint, nullptr, nullptr);
}

void enumerate_slots(SlotInfo *out) {
    if (!out) return;
    for (int s = 0; s < kNumSlots; ++s) {
        char hint[kHintBytes] = {0};
        // Heap-allocate the thumb buffer for this slot so SlotInfo
        // can hand back a stable pointer to save_menu.cpp's paint
        // loop.  4 × 4608 B = ~18 KB transient during the save menu;
        // freed by release_slots() on menu exit.  Heap rather than
        // BSS because BSS budget is tight; this only lives during
        // the save UI.
        uint16_t *thumb_buf = (uint16_t *)std::malloc(kThumbBytes);
        const bool ok = read_slot_meta(s, hint, nullptr, thumb_buf);
        out[s].occupied = ok;
        if (ok) {
            out[s].thumb = thumb_buf;
            std::memcpy(out[s].hint, hint, kHintBytes);
        } else {
            // Slot empty (or read failed) — release the unused buf
            // immediately so we don't leak it.
            std::free(thumb_buf);
            out[s].thumb = nullptr;
            out[s].hint[0] = '\0';
        }
    }
}

void release_slots(SlotInfo *out) {
    if (!out) return;
    for (int s = 0; s < kNumSlots; ++s) {
        if (out[s].thumb) {
            // const_cast — backend owns the heap buffer it handed back
            // through the const-pointer view in SlotInfo.
            std::free(const_cast<uint16_t *>(out[s].thumb));
            out[s].thumb = nullptr;
        }
    }
}

}  // namespace save_backend
}  // namespace tsb
