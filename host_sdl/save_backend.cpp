// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SDL host save backend.  One file per slot.  Each
// file mirrors the device layout exactly: 16-byte header + thumbnail
// + hint + SCUMM payload.  Layout matches device so the save_menu UI
// reads slot metadata via the same accessors.

#include "save_backend.h"
#include "common/stream.h"

#include <cstdio>
#include <cstring>

namespace tsb {
namespace save_backend {

namespace {

constexpr uint32_t kHeaderMagic   = 0x53425354u;
constexpr uint16_t kHeaderVersion = 3;  // v2→v3: 64×40 thumb → 64×36

struct __attribute__((packed)) Header {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t payload_size;
    uint32_t reserved2;
};
static_assert(sizeof(Header) == 16, "save header must be 16 bytes");

constexpr uint32_t kMetaOffset   = sizeof(Header);
constexpr uint32_t kHintOffset   = kMetaOffset + kThumbBytes;
constexpr uint32_t kPayloadStart = kHintOffset + kHintBytes;

void slot_path(int slot, char *out, size_t outsz) {
    std::snprintf(out, outsz, "thumbyscummby_slot%d.sav", slot);
}

// Cached metadata block per slot — kept in RAM so enumerate_slots()
// can return SlotInfo::thumb pointers that stay valid between calls
// without holding file handles.  Loaded lazily on first
// enumerate_slots(); refreshed after each save.
struct SlotCache {
    bool occupied;
    uint16_t thumb[kThumbW * kThumbH];
    char hint[kHintBytes];
};

SlotCache g_cache[kNumSlots] = {};
bool g_cache_loaded = false;

void load_slot_cache(int slot) {
    SlotCache &c = g_cache[slot];
    c.occupied = false;
    c.hint[0] = '\0';

    char path[64];
    slot_path(slot, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;

    Header hdr{};
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return;
    }
    if (hdr.magic != kHeaderMagic || hdr.version != kHeaderVersion) {
        fclose(f);
        return;
    }

    if (fread(c.thumb, 1, kThumbBytes, f) != kThumbBytes) {
        fclose(f);
        return;
    }
    if (fread(c.hint, 1, kHintBytes, f) != kHintBytes) {
        fclose(f);
        return;
    }
    c.hint[kHintBytes - 1] = '\0';
    c.occupied = true;
    fclose(f);
}

void ensure_cache_loaded() {
    if (g_cache_loaded) return;
    for (int s = 0; s < kNumSlots; ++s) load_slot_cache(s);
    g_cache_loaded = true;
}

// FILE* writer that pre-writes header + thumbnail + hint, then yields
// to the caller for the payload.  On destruction, seeks back to fix
// up the header payload_size.
class FileWriteStream : public Common::SeekableWriteStream {
public:
    FileWriteStream(FILE *f, int slot, const uint16_t *thumb, const char *hint)
        : _f(f), _slot(slot), _err(false), _payload_bytes(0) {
        // Reserve header.
        Header hdr{};
        fwrite(&hdr, 1, sizeof(hdr), _f);

        // Thumbnail.
        if (thumb) {
            fwrite(thumb, 1, kThumbBytes, _f);
        } else {
            uint8_t zero[256] = {0};
            uint32_t left = kThumbBytes;
            while (left > 0) {
                uint32_t chunk = left > sizeof(zero) ? sizeof(zero) : left;
                fwrite(zero, 1, chunk, _f);
                left -= chunk;
            }
        }

        // Hint.
        char hintbuf[kHintBytes];
        std::memset(hintbuf, 0, sizeof(hintbuf));
        if (hint) std::strncpy(hintbuf, hint, sizeof(hintbuf) - 1);
        fwrite(hintbuf, 1, kHintBytes, _f);
    }

    ~FileWriteStream() override {
        finalize();
    }

    uint32 write(const void *dataPtr, uint32 dataSize) override {
        if (!_f) return 0;
        size_t n = fwrite(dataPtr, 1, dataSize, _f);
        if (n != dataSize) _err = true;
        _payload_bytes += (uint32)n;
        return (uint32)n;
    }

    bool flush() override {
        if (!_f) return false;
        return fflush(_f) == 0;
    }

    void finalize() override {
        if (!_f) return;
        fflush(_f);
        // Measure actual on-disk size via ftell rather than trusting
        // _payload_bytes — Common::WriteStream callers can route writes
        // through paths that bypass our write() override (e.g. base
        // class buffered writes), and any such drift would otherwise
        // make load truncate.
        fseek(_f, 0, SEEK_END);
        const long actual_end = ftell(_f);
        Header hdr{};
        hdr.magic = kHeaderMagic;
        hdr.version = kHeaderVersion;
        hdr.payload_size = (uint32_t)(actual_end - (long)sizeof(hdr));
        fseek(_f, 0, SEEK_SET);
        fwrite(&hdr, 1, sizeof(hdr), _f);
        fclose(_f);
        _f = nullptr;
        // Refresh cache so enumerate_slots() reflects the new save.
        load_slot_cache(_slot);
    }

    int64 pos() const override { return (int64)_payload_bytes; }
    bool seek(int64 offset, int whence) override { _err = true; return false; }
    int64 size() const override { return (int64)_payload_bytes; }
    bool err() const override { return _err; }
    void clearErr() override { _err = false; }

private:
    FILE *_f;
    int   _slot;
    bool  _err;
    uint32 _payload_bytes;
};

class FileReadStream : public Common::SeekableReadStream {
public:
    FileReadStream(FILE *f, uint32 payload_size)
        : _f(f), _size(payload_size), _eos(false), _err(false) {}
    ~FileReadStream() override { if (_f) fclose(_f); }

    uint32 read(void *dataPtr, uint32 dataSize) override {
        if (!_f) { _err = true; return 0; }
        size_t n = fread(dataPtr, 1, dataSize, _f);
        if (n < dataSize) {
            if (feof(_f)) _eos = true;
            else _err = true;
        }
        return (uint32)n;
    }

    bool eos() const override { return _eos; }
    bool err() const override { return _err; }
    void clearErr() override { _err = _eos = false; if (_f) clearerr(_f); }

    int64 pos() const override {
        if (!_f) return -1;
        return (int64)ftell(_f) - (int64)kPayloadStart;
    }

    bool seek(int64 offset, int whence) override {
        if (!_f) return false;
        long target = (whence == SEEK_SET)
                          ? (long)kPayloadStart + (long)offset
                          : 0;
        if (whence != SEEK_SET) {
            // Other modes uncommon — handle SEEK_CUR for completeness.
            if (whence == SEEK_CUR)
                target = ftell(_f) + (long)offset;
            else
                target = (long)kPayloadStart + (long)_size + (long)offset;
        }
        bool ok = (fseek(_f, target, SEEK_SET) == 0);
        if (ok) _eos = false;
        return ok;
    }

    int64 size() const override { return (int64)_size; }

private:
    FILE *_f;
    uint32 _size;
    bool _eos;
    bool _err;
};

}  // anonymous

Common::SeekableWriteStream *open_for_writing(int slot,
                                              const uint16_t *thumb,
                                              const char *hint) {
    if (slot < 0 || slot >= kNumSlots) return nullptr;
    char path[64];
    slot_path(slot, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return nullptr;
    return new FileWriteStream(f, slot, thumb, hint);
}

Common::SeekableReadStream *open_for_reading(int slot) {
    if (slot < 0 || slot >= kNumSlots) return nullptr;
    char path[64];
    slot_path(slot, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    Header hdr{};
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        hdr.magic != kHeaderMagic ||
        hdr.version != kHeaderVersion) {
        fclose(f);
        return nullptr;
    }
    // Skip thumbnail + hint, leaving stream at start of SCUMM payload.
    if (fseek(f, kPayloadStart, SEEK_SET) != 0) {
        fclose(f);
        return nullptr;
    }
    const uint32_t payload_size = hdr.payload_size - kThumbBytes - kHintBytes;
    return new FileReadStream(f, payload_size);
}

bool has_save(int slot) {
    ensure_cache_loaded();
    if (slot < 0 || slot >= kNumSlots) return false;
    return g_cache[slot].occupied;
}

void enumerate_slots(SlotInfo *out) {
    if (!out) return;
    ensure_cache_loaded();
    for (int s = 0; s < kNumSlots; ++s) {
        out[s].occupied = g_cache[s].occupied;
        if (out[s].occupied) {
            out[s].thumb = g_cache[s].thumb;
            std::memcpy(out[s].hint, g_cache[s].hint, kHintBytes);
        } else {
            out[s].thumb = nullptr;
            out[s].hint[0] = '\0';
        }
    }
}

}  // namespace save_backend
}  // namespace tsb
