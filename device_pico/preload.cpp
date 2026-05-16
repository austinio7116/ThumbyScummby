// Preload pipeline — see preload.h for overview.

#include "preload.h"
#include "pcv_install.h"

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
// fully present (no half-uploaded sets).  Returns false if the
// descriptor has no required files at all (e.g. the Indy3 placeholder
// entry pending V3_LFL resolver work), so the pipeline doesn't try
// to run on a descriptor with nothing to install.
bool required_files_present(const GameDescriptor &gd) {
    if (!gd.files) return false;
    bool saw_required = false;
    char path[64];
    for (const GameFile *gf = gd.files; gf->name; ++gf) {
        if (!gf->required) continue;
        saw_required = true;
        if (!join_path(path, sizeof(path), gd.subdir, gf->name)) return false;
        if (!file_exists(path)) return false;
    }
    return saw_required;
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

// "Already decrypted" sniff for safe migration of users who
// hand-decrypted (tools/decrypt_scumm_data.py) before the .thumbyscummby
// marker existed.  LucasArts chunk headers always contain uppercase
// ASCII tags within the first 8 bytes:
//   v4 SMALL_HEADER LEC: uint32 size + 'LE'/'FO'/'OB' at offset 4-5
//   v5 HD .000/.001:     'LECF' at offset 0-3 (big-endian tag)
// Plain bytes in those positions land in 'A'..'Z'.  XOR'd-by-0x69
// turns those same letters into 0x25..0x33 punctuation, so a
// decrypted file is guaranteed to expose ≥2 upper-ASCII letters
// in the first 8 bytes while an encrypted file exposes 0.
//
// The previous version only looked at bytes 0-1 — but in v4 .LEC
// those are the LOW 16 bits of the file size (e.g. 0x4A 0xC6 for
// DISK01.LEC), so the test missed already-plain v4 files and the
// XOR pass *re-encrypted* them.
bool looks_already_plain(const char *path) {
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK) return false;
    uint8_t buf[8] = {0};
    UINT br = 0;
    bool ok = (f_read(&fp, buf, sizeof(buf), &br) == FR_OK)
           && br == sizeof(buf);
    f_close(&fp);
    if (!ok) return false;
    int letters = 0;
    for (int i = 0; i < 8; ++i) {
        if (buf[i] >= 'A' && buf[i] <= 'Z') ++letters;
    }
    return letters >= 2;
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
    if (looks_already_plain(path)) {
        // Already-decrypted fast path — paint progress so the bar
        // still advances but stay out of the log; this is the common
        // case once the install has run once.
        const int overall_pct =
            ((file_index + 1) * 100) / (file_count > 0 ? file_count : 1);
        tsb::platform::preload_progress(display_name, short_name,
                                        overall_pct);
        return true;
    }

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

// ===========================================================================
// FAT12 reader for LucasArts floppy .img files
// ===========================================================================
//
// Minimal read-only parser hand-rolled rather than going through FatFs's
// multi-volume support (FF_VOLUMES is pinned at 1 across all ThumbyOne
// slots — see ThumbyOne/common/lib/fatfs/ffconf.h).  Assumes:
//
//   - FAT12 with a standard DOS BPB at sector 0
//   - 512-byte logical sectors (every LucasArts disk image)
//   - Files in the root directory only (no subdirs)
//   - 8.3 short filenames (no LFN entries)
//
// All we ever do with an .img is open it, list root entries, and copy
// the cluster chain of named files out to /scumm/<sub>/<filename>.
// ===========================================================================

namespace fat12 {

constexpr uint32_t kSectorBytes = 512;
constexpr uint32_t kDirEntryBytes = 32;
constexpr uint8_t  kAttrVolumeID = 0x08;
constexpr uint8_t  kAttrLFN      = 0x0F;     // long-filename entry
constexpr uint8_t  kAttrDirectory = 0x10;

struct __attribute__((packed)) BPB {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // ... rest of BPB ignored
};
static_assert(sizeof(BPB) >= 36, "BPB layout off");

struct Reader {
    FIL fp;
    bool opened = false;

    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t root_entries;
    uint16_t sectors_per_fat;

    uint32_t fat_first_sector;
    uint32_t root_first_sector;
    uint32_t data_first_sector;

    uint8_t  fat_cache[kSectorBytes];   // single-sector FAT cache
    int32_t  fat_cache_sector;           // -1 = empty

    bool open(const char *path) {
        if (f_open(&fp, path, FA_READ) != FR_OK) return false;
        opened = true;
        fat_cache_sector = -1;
        return read_bpb();
    }

    void close() {
        if (opened) { f_close(&fp); opened = false; }
    }

    bool read_sector(uint32_t lba, void *out) {
        if (f_lseek(&fp, (FSIZE_t)lba * kSectorBytes) != FR_OK) return false;
        UINT br = 0;
        if (f_read(&fp, out, kSectorBytes, &br) != FR_OK || br != kSectorBytes)
            return false;
        return true;
    }

    bool read_bpb() {
        uint8_t sec[kSectorBytes];
        if (!read_sector(0, sec)) return false;
        const BPB *b = reinterpret_cast<const BPB *>(sec);

        bytes_per_sector    = b->bytes_per_sector;
        sectors_per_cluster = b->sectors_per_cluster;
        root_entries        = b->root_entries;
        sectors_per_fat     = b->sectors_per_fat;

        if (bytes_per_sector != kSectorBytes) return false;
        if (sectors_per_cluster == 0) return false;
        if (b->num_fats == 0) return false;

        fat_first_sector  = b->reserved_sectors;
        root_first_sector = fat_first_sector + b->num_fats * sectors_per_fat;
        const uint32_t root_sectors =
            (root_entries * kDirEntryBytes + kSectorBytes - 1) / kSectorBytes;
        data_first_sector = root_first_sector + root_sectors;
        return true;
    }

    // FAT12 entry at index `cluster` is at byte offset (cluster * 3 / 2)
    // from the start of the FAT.  Each entry is 12 bits packed.
    uint16_t fat_entry(uint16_t cluster) {
        const uint32_t entry_byte = (uint32_t)cluster + ((uint32_t)cluster >> 1);
        const uint32_t entry_sec  = fat_first_sector + entry_byte / kSectorBytes;
        const uint32_t entry_off  = entry_byte % kSectorBytes;

        if ((int32_t)entry_sec != fat_cache_sector) {
            if (!read_sector(entry_sec, fat_cache)) return 0xFFF;
            fat_cache_sector = (int32_t)entry_sec;
        }

        uint16_t val;
        if (entry_off + 1 < kSectorBytes) {
            val = (uint16_t)fat_cache[entry_off]
                | ((uint16_t)fat_cache[entry_off + 1] << 8);
        } else {
            // Entry straddles a sector boundary — read the next.
            uint8_t next[kSectorBytes];
            if (!read_sector(entry_sec + 1, next)) return 0xFFF;
            val = (uint16_t)fat_cache[entry_off]
                | ((uint16_t)next[0] << 8);
        }
        if (cluster & 1) val >>= 4;
        else             val &= 0x0FFF;
        return val;
    }

    uint32_t cluster_to_lba(uint16_t cluster) const {
        return data_first_sector
             + (uint32_t)(cluster - 2) * sectors_per_cluster;
    }

    // Build "FOO     LFL" from 11-byte raw directory entry → "FOO.LFL".
    // Returns false if first byte is 0x00 (end-of-dir) or 0xE5 (deleted).
    static bool format_name(const uint8_t *raw11, char *out, size_t outsz) {
        if (raw11[0] == 0x00 || raw11[0] == 0xE5) return false;
        if (outsz < 13) return false;
        int o = 0;
        for (int i = 0; i < 8; ++i) {
            uint8_t c = raw11[i];
            if (c == ' ') break;
            out[o++] = (char)c;
        }
        bool has_ext = false;
        for (int i = 8; i < 11; ++i) {
            if (raw11[i] != ' ') { has_ext = true; break; }
        }
        if (has_ext) {
            out[o++] = '.';
            for (int i = 8; i < 11; ++i) {
                uint8_t c = raw11[i];
                if (c == ' ') break;
                out[o++] = (char)c;
            }
        }
        out[o] = '\0';
        return true;
    }

    // Iterator: for each root entry that's a real file, callback with
    // (name, size, start_cluster).  Returns true on clean walk.
    template <typename CB>
    bool walk_root(CB cb) {
        const uint32_t entries_per_sector = kSectorBytes / kDirEntryBytes;
        const uint32_t root_sectors =
            (root_entries * kDirEntryBytes + kSectorBytes - 1) / kSectorBytes;
        uint8_t sec[kSectorBytes];
        for (uint32_t rs = 0; rs < root_sectors; ++rs) {
            if (!read_sector(root_first_sector + rs, sec)) return false;
            for (uint32_t e = 0; e < entries_per_sector; ++e) {
                const uint8_t *ent = sec + e * kDirEntryBytes;
                if (ent[0] == 0x00) return true;         // end of dir
                if (ent[0] == 0xE5) continue;            // deleted
                const uint8_t attr = ent[11];
                if (attr & (kAttrVolumeID | kAttrDirectory)) continue;
                if (attr == kAttrLFN) continue;           // ignore LFN
                char name[13];
                if (!format_name(ent, name, sizeof(name))) continue;
                const uint16_t start_cluster =
                    (uint16_t)ent[26] | ((uint16_t)ent[27] << 8);
                const uint32_t size =
                    (uint32_t)ent[28]
                  | ((uint32_t)ent[29] << 8)
                  | ((uint32_t)ent[30] << 16)
                  | ((uint32_t)ent[31] << 24);
                if (!cb(name, size, start_cluster)) return true;
            }
        }
        return true;
    }

    // Copy a file's contents to `dest_path` by walking its cluster chain.
    // 512-byte sector buffer reused across reads/writes.  Stops at `size`
    // bytes (last cluster is partial); also stops at EOC (clusters >=
    // 0xFF8).  Returns true on success.
    bool extract_to(uint16_t start_cluster, uint32_t size,
                    const char *dest_path) {
        FIL dst;
        if (f_open(&dst, dest_path,
                   FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;

        uint8_t buf[kSectorBytes];
        const uint32_t cluster_bytes = sectors_per_cluster * kSectorBytes;
        uint32_t written = 0;
        uint16_t curr = start_cluster;
        bool ok = true;

        while (curr >= 2 && curr < 0xFF8 && written < size) {
            const uint32_t cluster_lba = cluster_to_lba(curr);
            const uint32_t want_in_cluster =
                (size - written) < cluster_bytes
                ? (size - written) : cluster_bytes;
            uint32_t produced = 0;
            for (uint8_t s = 0; s < sectors_per_cluster && produced < want_in_cluster; ++s) {
                if (!read_sector(cluster_lba + s, buf)) { ok = false; break; }
                const uint32_t take = (want_in_cluster - produced) < kSectorBytes
                                    ? (want_in_cluster - produced) : kSectorBytes;
                UINT bw = 0;
                if (f_write(&dst, buf, take, &bw) != FR_OK || bw != take) {
                    ok = false; break;
                }
                produced += take;
                written  += take;
            }
            if (!ok) break;
            curr = fat_entry(curr);
        }

        f_close(&dst);
        if (!ok) f_unlink(dest_path);
        return ok && written == size;
    }
};

}  // namespace fat12

// Look up a name (case-insensitive) in the descriptor's required-or-
// optional file list.  Returns null if the name doesn't match any
// known game file.
const GameFile *match_inner_name(const GameDescriptor &gd, const char *name) {
    if (!gd.files) return nullptr;
    for (const GameFile *gf = gd.files; gf->name; ++gf) {
        if (strcasecmp(gf->name, name) == 0) return gf;
    }
    return nullptr;
}

// Extract every recognised file from every .img under /scumm/<sub>/.
// Returns true if we extracted at least one file (so the caller knows
// to rescan / re-evaluate marker state).  Deletes .imgs after a
// successful extraction so disk space frees up and we don't keep
// re-processing them.
bool extract_imgs_for(const GameDescriptor &gd) {
    char dirpath[40];
    if (std::snprintf(dirpath, sizeof(dirpath), "/scumm/%s", gd.subdir)
        >= (int)sizeof(dirpath)) return false;

    DIR d;
    if (f_opendir(&d, dirpath) != FR_OK) return false;

    bool extracted_anything = false;
    FILINFO fi;
    for (;;) {
        FRESULT rr = f_readdir(&d, &fi);
        if (rr != FR_OK || fi.fname[0] == 0) break;
        const char *dot = std::strrchr(fi.fname, '.');
        if (!dot || strcasecmp(dot, ".img") != 0) continue;

        char imgpath[64];
        if (std::snprintf(imgpath, sizeof(imgpath), "%s/%s",
                          dirpath, fi.fname) >= (int)sizeof(imgpath)) continue;

        tsb::platform::log("[preload] img %s\n", fi.fname);
        tsb::platform::preload_progress(gd.display_name, fi.fname, 0);

        fat12::Reader r;
        if (!r.open(imgpath)) {
            tsb::platform::log("[preload] img open fail\n");
            continue;
        }

        // Two passes: first one logs / matches, second one extracts.
        // Pass 1 just collects.  Pass 2 actually copies.  In practice
        // we fold them into a single walk_root callback that decides
        // per-entry.
        bool any_extracted_here = false;
        bool walk_ok = r.walk_root([&](const char *name, uint32_t size,
                                       uint16_t start_cluster) -> bool {
            const GameFile *gf = match_inner_name(gd, name);
            if (!gf) return true;
            if (size == 0 || start_cluster < 2) return true;
            char dst[64];
            if (std::snprintf(dst, sizeof(dst), "%s/%s", dirpath, name)
                >= (int)sizeof(dst)) return true;
            FILINFO existing;
            if (f_stat(dst, &existing) == FR_OK && existing.fsize == size) {
                // Same-size file already extracted (e.g. previous run); skip.
                return true;
            }
            tsb::platform::preload_progress(gd.display_name, name, 0);
            if (!r.extract_to(start_cluster, size, dst)) {
                tsb::platform::log("[preload] extract fail %s\n", name);
                return true;   // keep walking — other files may still work
            }
            any_extracted_here = true;
            return true;
        });

        r.close();
        if (walk_ok && any_extracted_here) {
            // Drop the .img — its contents are now loose in the
            // game subdir, ready for decrypt.
            f_unlink(imgpath);
            extracted_anything = true;
        }
    }
    f_closedir(&d);
    return extracted_anything;
}

bool run_one(const GameDescriptor &gd) {
    if (marker_present(gd.subdir)) return false;

    // Phase 3.B: if .img files are present in /scumm/<sub>/, mount and
    // extract their contents into the subdir before the decrypt pass
    // sees the files.  This is a no-op if no .imgs are there.
    (void)extract_imgs_for(gd);

    if (!required_files_present(gd)) return false;

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
    return true;
}

}  // anonymous

bool maybe_run() {
    bool did_work = false;
    // Phase 3.B (new): if any .img install-disks are sitting in
    // /scumm/, parse them as LucasArts PCV/LFG! archives, decompress
    // via DCL, write the game data into /scumm/<game>/, and f_unlink
    // each .img once consumed.
    if (install_pcv_imgs()) {
        did_work = true;
    }
    for (int i = 0; i < tsb::kGameTableCount; ++i) {
        if (run_one(tsb::kGameTable[i])) did_work = true;
    }
    return did_work;
}

}  // namespace preload
}  // namespace tsb
