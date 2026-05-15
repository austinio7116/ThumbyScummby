// Preload pipeline — see preload.h for overview.

#include "preload.h"

#include "platform.h"
#include "game_table.h"

extern "C" {
#include "ff.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace tsb {
namespace preload {

namespace {

constexpr const char *kMarkerName = ".thumbyscummby";
constexpr size_t      kChunkBytes = 4096;
constexpr uint32_t    kProgressIntervalMs = 80;  // throttle paint calls

// MI2 Mix-N-Mojo skip — see tools/pack_device.py for the rationale.
// 26-byte SCUMM bytecode signature in MONKEY2.001; flipping byte +3
// (the `if Local[0] == 0` immediate) from 0x00 to 0x01 jumps the
// protection block.
static const uint8_t kMI2ProtSig[26] = {
    0x48, 0x00, 0x40, 0x00, 0x00, 0x13, 0x00,
    0x33, 0x03, 0x00, 0x00, 0xc8, 0x00,
    0x0a, 0x82, 0xff,
    0x80,
    0x68, 0x00, 0x00, 0x82,
    0x28, 0x00, 0x00, 0xf6, 0xff,
};

// Build /scumm/<subdir>/<file>.  Returns false on overflow.
bool join_path(char *out, size_t outsz, const char *subdir, const char *file) {
    int n = std::snprintf(out, outsz, "/scumm/%s/%s", subdir, file);
    return n > 0 && (size_t)n < outsz;
}

bool file_exists(const char *path) {
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

bool marker_present(const char *subdir) {
    char path[64];
    if (!join_path(path, sizeof(path), subdir, kMarkerName)) return false;
    return file_exists(path);
}

// True if every required file (per the descriptor) exists in the
// subdir.  Used to gate preload — we only run if the raw files are
// fully present (no half-uploaded sets).
bool required_files_present(const GameDescriptor &gd) {
    if (!gd.files) return false;
    char path[64];
    for (const GameFile *gf = gd.files; gf->name; ++gf) {
        if (!gf->required) continue;
        if (!join_path(path, sizeof(path), gd.subdir, gf->name)) return false;
        if (!file_exists(path)) return false;
    }
    return true;
}

bool write_marker(const char *subdir, const GameDescriptor &gd) {
    char path[64];
    if (!join_path(path, sizeof(path), subdir, kMarkerName)) return false;
    FIL fp;
    if (f_open(&fp, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    char body[64];
    int n = std::snprintf(body, sizeof(body),
                          "v=1\nsubdir=%s\nvariant=%d\n",
                          gd.subdir, (int)gd.variant);
    UINT bw = 0;
    bool ok = (n > 0) && f_write(&fp, body, (UINT)n, &bw) == FR_OK && bw == (UINT)n;
    f_close(&fp);
    return ok;
}

// XOR-decrypt a file in place.  Open for r/w, read a chunk, XOR each
// byte, seek back, write.  Returns false on I/O error; the caller
// then refuses to mark the install as done so the next boot retries.
//
// `display_name` is just for the progress UI; `file_index` /
// `file_count` is used to map per-file progress into an overall %
// (file N starts at N/count, finishes at (N+1)/count).
bool decrypt_in_place(const char *path,
                      uint8_t xor_byte,
                      const char *display_name,
                      const char *short_name,
                      int file_index,
                      int file_count) {
    if (xor_byte == 0) return true;

    FIL fp;
    if (f_open(&fp, path, FA_READ | FA_WRITE) != FR_OK) return false;
    const uint32_t total = (uint32_t)f_size(&fp);
    if (total == 0) { f_close(&fp); return true; }

    uint8_t *buf = (uint8_t *)std::malloc(kChunkBytes);
    if (!buf) { f_close(&fp); return false; }

    bool ok = true;
    uint32_t pos = 0;
    uint32_t last_paint_pct = 0xFFFFFFFFu;

    while (pos < total) {
        const uint32_t want = (total - pos) < kChunkBytes
                              ? (total - pos) : kChunkBytes;
        if (f_lseek(&fp, pos) != FR_OK) { ok = false; break; }
        UINT br = 0;
        if (f_read(&fp, buf, want, &br) != FR_OK || br != want) {
            ok = false; break;
        }
        for (UINT i = 0; i < br; ++i) buf[i] ^= xor_byte;
        if (f_lseek(&fp, pos) != FR_OK) { ok = false; break; }
        UINT bw = 0;
        if (f_write(&fp, buf, br, &bw) != FR_OK || bw != br) {
            ok = false; break;
        }
        pos += br;

        // Throttle progress paints — each is a full-frame LCD blit.
        const int file_pct = (pos * 100) / total;
        const int overall_pct =
            (file_index * 100 + file_pct) / (file_count > 0 ? file_count : 1);
        if ((uint32_t)overall_pct != last_paint_pct) {
            tsb::platform::preload_progress(display_name, short_name,
                                            overall_pct);
            last_paint_pct = (uint32_t)overall_pct;
        }
    }

    f_close(&fp);
    std::free(buf);
    return ok;
}

// Search a decrypted MONKEY2.001 for the Mix-N-Mojo signature and
// patch byte +3 from 0x00 to 0x01.  Idempotent — re-running on a
// patched file is a no-op (the signature no longer matches).
bool apply_mi2_mojo(const char *path) {
    FIL fp;
    if (f_open(&fp, path, FA_READ | FA_WRITE) != FR_OK) return false;
    const uint32_t total = (uint32_t)f_size(&fp);
    if (total < sizeof(kMI2ProtSig)) { f_close(&fp); return true; }

    constexpr size_t kBuf = 8192;
    uint8_t *buf = (uint8_t *)std::malloc(kBuf);
    if (!buf) { f_close(&fp); return false; }
    const size_t kSigLen = sizeof(kMI2ProtSig);

    bool patched = false;
    bool ok = true;
    uint32_t pos = 0;
    // Read overlapping chunks so the signature can straddle a
    // boundary; back up by (kSigLen-1) at each step.
    while (pos < total) {
        const uint32_t want = (total - pos) < kBuf ? (total - pos) : kBuf;
        if (f_lseek(&fp, pos) != FR_OK) { ok = false; break; }
        UINT br = 0;
        if (f_read(&fp, buf, want, &br) != FR_OK) { ok = false; break; }
        if (br < kSigLen) break;
        for (UINT i = 0; i + kSigLen <= br; ++i) {
            if (std::memcmp(buf + i, kMI2ProtSig, kSigLen) == 0) {
                const uint32_t file_off = pos + i + 3;
                uint8_t new_byte = 0x01;
                if (f_lseek(&fp, file_off) != FR_OK) { ok = false; break; }
                UINT bw = 0;
                if (f_write(&fp, &new_byte, 1, &bw) != FR_OK || bw != 1) {
                    ok = false; break;
                }
                patched = true;
                tsb::platform::log("[preload] mi2-mojo @0x%lx\n",
                                   (unsigned long)file_off);
                break;
            }
        }
        if (!ok || patched) break;
        // advance, keeping kSigLen-1 overlap
        if (br < want) break;       // EOF
        pos += (uint32_t)(br - (kSigLen - 1));
    }
    std::free(buf);
    f_close(&fp);
    (void)patched;   // informational only
    return ok;
}

bool run_one(const GameDescriptor &gd) {
    if (marker_present(gd.subdir)) return false;
    if (!required_files_present(gd)) return false;

    tsb::platform::log("[preload] start %s\n", gd.subdir);

    // Count files that actually need a decrypt pass (xor_byte != 0)
    // so progress percentage tracks real work, not 8 helper-LFL
    // no-ops.
    int n_xor = 0;
    for (const GameFile *gf = gd.files; gf->name; ++gf) {
        if (gf->xor_byte != 0) ++n_xor;
    }

    tsb::platform::preload_progress(gd.display_name, "...", 0);

    int idx = 0;
    char path[64];
    for (const GameFile *gf = gd.files; gf->name; ++gf) {
        if (gf->xor_byte == 0) continue;
        if (!join_path(path, sizeof(path), gd.subdir, gf->name)) return false;
        if (!file_exists(path)) {
            // Optional missing files skipped silently; required files
            // were already validated above so this only fires for
            // optional ones marked xor_byte != 0 (none today).
            ++idx;
            continue;
        }
        if (!decrypt_in_place(path, gf->xor_byte, gd.display_name,
                              gf->name, idx, n_xor > 0 ? n_xor : 1)) {
            tsb::platform::log("[preload] fail %s\n", gf->name);
            return false;
        }
        ++idx;
    }

    // MI2 Mojo patch — only meaningful for the MI2 descriptor, but
    // safe to call on others (signature won't match).
    if (gd.hd_basename && std::strcmp(gd.hd_basename, "monkey2") == 0) {
        char p001[64];
        if (join_path(p001, sizeof(p001), gd.subdir, "monkey2.001")) {
            (void)apply_mi2_mojo(p001);
        }
    }

    if (!write_marker(gd.subdir, gd)) {
        tsb::platform::log("[preload] mark fail\n");
        return false;
    }

    tsb::platform::log("[preload] done %s\n", gd.subdir);
    return true;
}

}  // anonymous

bool maybe_run() {
    bool did_work = false;
    for (int i = 0; i < tsb::kGameTableCount; ++i) {
        if (run_one(tsb::kGameTable[i])) did_work = true;
    }
    return did_work;
}

}  // namespace preload
}  // namespace tsb
