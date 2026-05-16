// PCV install pipeline — Stage B.  Stage A is preserved at commit
// 8acaf53 if you need to bisect.  Differences from Stage A:
//
//   * Once a PCV has been located inside a .img (via FatFs at probe
//     time), the .img is NOT read through FatFs anymore — we walk
//     the .img's outer FAT16 chain ourselves with disk_read of one
//     cluster at a time.  As each outer cluster is consumed, we
//     immediately free it on the shared FAT16 via OuterFatCache so
//     the .img shrinks while it's being read.
//
//   * At the end of each .img we clear its directory entry manually
//     (the file's chain is already broken — FatFs's f_unlink would
//     not survive that), so the .img is fully gone from /scumm/.
//
// This keeps peak shared-FAT usage during install down to roughly
// (largest extracted file so far) + (last KB of the .img we're
// currently reading) instead of the full pile of .imgs.
//
// Stage B assumes the PCV's inner FAT12 chain and the .img's outer
// FAT16 chain are both sequential (cluster N → cluster N+1).  This
// holds for fresh LucasArts install disks copied onto a fresh shared
// FAT, which is the only environment we support.  We verify at probe
// time and bail with a log message if not.

#include "pcv_install.h"

#include "dcl.h"
#include "game_table.h"
#include "platform.h"

extern "C" {
#include "ff.h"
#include "diskio.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>

// Defined in platform_pico.cpp — exposes the mounted shared-FAT
// volume so we can do raw FAT16 / dir-entry manipulation.
namespace tsb { namespace platform_pico {
FATFS *get_fatfs();
}}

namespace tsb {
namespace preload {

namespace {

// ===========================================================================
// FAT12 helpers — operating on a single outer .img file via FatFs f_read.
// Used at probe time only.  After probing, we close the FIL and switch to
// raw outer-cluster reads (see RawImgReader below).
// ===========================================================================

struct ImgGeometry {
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t sectors_per_fat;
    uint32_t fat_first_sec;
    uint32_t root_first_sec;
    uint32_t data_first_sec;
};

struct ImgFatCache {
    uint8_t  buf[512];
    int32_t  sector;     // -1 if empty
};

static bool read_img_sector(FIL *fp, uint32_t sec, uint8_t *out) {
    if (f_lseek(fp, (FSIZE_t)sec * 512) != FR_OK) return false;
    UINT br = 0;
    if (f_read(fp, out, 512, &br) != FR_OK || br != 512) return false;
    return true;
}

static bool parse_geometry(FIL *fp, ImgGeometry *g) {
    uint8_t sec[512];
    if (!read_img_sector(fp, 0, sec)) return false;
    g->bytes_per_sector    = (uint16_t)(sec[11] | (sec[12] << 8));
    g->sectors_per_cluster = sec[13];
    g->reserved_sectors    = (uint16_t)(sec[14] | (sec[15] << 8));
    g->num_fats            = sec[16];
    g->root_entries        = (uint16_t)(sec[17] | (sec[18] << 8));
    g->sectors_per_fat     = (uint16_t)(sec[22] | (sec[23] << 8));
    if (g->bytes_per_sector != 512 || g->sectors_per_cluster == 0
            || g->num_fats == 0) {
        return false;
    }
    g->fat_first_sec  = g->reserved_sectors;
    g->root_first_sec = g->fat_first_sec + g->num_fats * g->sectors_per_fat;
    g->data_first_sec = g->root_first_sec
                      + (g->root_entries * 32u + 511u) / 512u;
    return true;
}

static uint16_t fat12_next(FIL *fp, const ImgGeometry &g, ImgFatCache &c,
                           uint16_t cluster) {
    uint32_t off  = (uint32_t)cluster + ((uint32_t)cluster >> 1);
    uint32_t sec  = g.fat_first_sec + off / 512;
    uint32_t boff = off % 512;
    if ((int32_t)sec != c.sector) {
        if (!read_img_sector(fp, sec, c.buf)) return 0xFFFu;
        c.sector = (int32_t)sec;
    }
    uint16_t raw;
    if (boff + 1 < 512) {
        raw = (uint16_t)(c.buf[boff] | (c.buf[boff + 1] << 8));
    } else {
        uint8_t spare[512];
        if (!read_img_sector(fp, sec + 1, spare)) return 0xFFFu;
        raw = (uint16_t)(c.buf[boff] | (spare[0] << 8));
    }
    return (cluster & 1) ? (raw >> 4) : (raw & 0x0FFFu);
}

// Walk the .img's root dir.  cb(name11, start_cluster, size) returns
// false to stop early.
template <typename CB>
static bool walk_img_root(FIL *fp, const ImgGeometry &g, CB cb) {
    const uint32_t root_secs = (g.root_entries * 32u + 511u) / 512u;
    uint8_t buf[512];
    for (uint32_t rs = 0; rs < root_secs; ++rs) {
        if (!read_img_sector(fp, g.root_first_sec + rs, buf)) return false;
        for (uint32_t e = 0; e < 16; ++e) {
            const uint8_t *p = buf + e * 32;
            if (p[0] == 0) return true;
            if (p[0] == 0xE5) continue;
            if ((p[11] & 0x08) || p[11] == 0x0F) continue;
            char name11[12];
            std::memcpy(name11, p, 11);
            name11[11] = 0;
            uint16_t sc = (uint16_t)(p[26] | (p[27] << 8));
            uint32_t sz = (uint32_t)(p[28] | (p[29] << 8)
                                  | (p[30] << 16) | (p[31] << 24));
            if (!cb(name11, sc, sz)) return true;
        }
    }
    return true;
}

// Walk the PCV's inner FAT12 chain and report (a) how many transitions
// were sequential (cluster N → N+1) and (b) the first offending pair
// if any.  Stage B's reader needs strict sequential to work, but we
// log details (rather than silently rejecting) so the LOG viewer
// shows what kind of layout we actually hit.
struct InnerChainStats {
    uint32_t expected_transitions;
    uint32_t observed_sequential;
    bool     ok;
    uint16_t bad_from;
    uint16_t bad_to;
};
static InnerChainStats walk_inner_chain(FIL *fp, const ImgGeometry &g,
                                        uint16_t start, uint32_t size) {
    InnerChainStats s = {};
    const uint32_t cb = (uint32_t)g.sectors_per_cluster * 512u;
    s.expected_transitions = (size > cb) ? ((size + cb - 1) / cb) - 1 : 0;
    ImgFatCache fc{}; fc.sector = -1;
    uint16_t cur = start;
    s.ok = true;
    for (uint32_t i = 0; i < s.expected_transitions; ++i) {
        uint16_t next = fat12_next(fp, g, fc, cur);
        if (next != (uint16_t)(cur + 1)) {
            if (s.ok) {
                s.ok = false;
                s.bad_from = cur;
                s.bad_to   = next;
            }
            // Stop walking — we can't trust the chain beyond this
            // point if it's hitting EOC or garbage.
            if (next < 2 || next >= 0xFF8) break;
        } else {
            s.observed_sequential++;
        }
        cur = next;
    }
    return s;
}

// ===========================================================================
// PCV chain discovery
// ===========================================================================

struct PcvPattern {
    const char *prefix;
    const char *suffix;
    int         letter_off;
};
static const PcvPattern kPatternMI1   = { "PCV10__", "MI1", 7 };
static const PcvPattern kPatternMI2   = { "PCV10__", "MI2", 7 };
static const PcvPattern kPatternIndy4 = { "PCVIQA_", "ND4", 7 };

static const GameDescriptor *match_pcv_pattern(const char *name11,
                                               char *out_letter) {
    auto try_match = [&](const PcvPattern &pat) -> bool {
        if (std::strncmp(name11, pat.prefix, 7) != 0) return false;
        if (std::strncmp(name11 + 8, pat.suffix, 3) != 0) return false;
        *out_letter = name11[pat.letter_off];
        return true;
    };
    for (int i = 0; i < tsb::kGameTableCount; ++i) {
        const auto &gd = tsb::kGameTable[i];
        if (std::strcmp(gd.subdir, "mi1") == 0   && try_match(kPatternMI1))   return &gd;
        if (std::strcmp(gd.subdir, "mi2") == 0   && try_match(kPatternMI2))   return &gd;
        if (std::strcmp(gd.subdir, "indy4") == 0 && try_match(kPatternIndy4)) return &gd;
    }
    return nullptr;
}

struct PcvSource {
    char     img_path[64];
    char     inner_name[12];
    char     chain_letter;
    uint16_t inner_start_cluster;
    uint32_t inner_size;
    ImgGeometry geom;

    // Stage B: outer-FAT info captured from FatFs at probe time so we
    // can later walk the .img's outer cluster chain ourselves.
    DWORD    outer_first_cluster;
    uint32_t outer_size_bytes;
    char     parent_dir[48];   // e.g. "/scumm" or "/scumm/mi2"
    char     basename_83[12];  // 8.3 form for outer dir-entry match
};

struct PcvChain {
    const GameDescriptor *gd;
    PcvSource sources[8];
    int       count;
};

// Convert "DISK01.IMG" or "disk01.img" into the FAT 8.3 dir-entry
// representation: 8 name chars + 3 ext chars, uppercase, space-padded.
static void to_83(const char *fname, char *out11) {
    std::memset(out11, ' ', 11);
    int oi = 0;
    const char *p = fname;
    while (*p && *p != '.' && oi < 8) {
        char c = *p++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out11[oi++] = c;
    }
    while (*p && *p != '.') ++p;
    if (*p == '.') ++p;
    oi = 8;
    while (*p && oi < 11) {
        char c = *p++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out11[oi++] = c;
    }
}

static void split_path(const char *path, char *parent_out, size_t parent_size,
                       char *basename_out, size_t basename_size) {
    const char *slash = std::strrchr(path, '/');
    if (!slash) {
        std::snprintf(parent_out, parent_size, ".");
        std::snprintf(basename_out, basename_size, "%s", path);
        return;
    }
    size_t dl = (size_t)(slash - path);
    if (dl == 0) {
        std::snprintf(parent_out, parent_size, "/");
    } else {
        if (dl >= parent_size) dl = parent_size - 1;
        std::memcpy(parent_out, path, dl);
        parent_out[dl] = 0;
    }
    std::snprintf(basename_out, basename_size, "%s", slash + 1);
}

static bool probe_img(const char *path, PcvSource *out,
                      const GameDescriptor **out_gd) {
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK) return false;
    ImgGeometry g;
    if (!parse_geometry(&fp, &g)) { f_close(&fp); return false; }

    bool found = false;
    walk_img_root(&fp, g, [&](const char *name11, uint16_t sc,
                              uint32_t sz) -> bool {
        char letter = 0;
        const GameDescriptor *gd = match_pcv_pattern(name11, &letter);
        if (!gd) return true;
        std::memcpy(out->inner_name, name11, 12);
        out->chain_letter        = letter;
        out->inner_start_cluster = sc;
        out->inner_size          = sz;
        out->geom                = g;
        std::snprintf(out->img_path, sizeof(out->img_path), "%s", path);
        *out_gd                  = gd;
        found = true;
        return false;
    });

    if (found) {
        InnerChainStats st = walk_inner_chain(&fp, g, out->inner_start_cluster,
                                              out->inner_size);
        if (!st.ok) {
            tsb::platform::log("[pcv] non-seq %s at %u->%u\n",
                               out->inner_name,
                               (unsigned)st.bad_from, (unsigned)st.bad_to);
            found = false;
        } else {
            out->outer_first_cluster = fp.obj.sclust;
            out->outer_size_bytes    = (uint32_t)fp.obj.objsize;
            char basename[24];
            split_path(path, out->parent_dir, sizeof(out->parent_dir),
                       basename, sizeof(basename));
            to_83(basename, out->basename_83);
            out->basename_83[11] = 0;
        }
    }

    f_close(&fp);
    return found;
}

static bool discover_chains(PcvChain *chains, int max_chains, int *n_chains) {
    *n_chains = 0;

    auto insert_into_chain = [&](const GameDescriptor *gd, const PcvSource &src) {
        for (int i = 0; i < *n_chains; ++i) {
            if (chains[i].gd == gd) {
                if (chains[i].count >= 8) return;
                chains[i].sources[chains[i].count++] = src;
                return;
            }
        }
        if (*n_chains >= max_chains) return;
        PcvChain &c = chains[(*n_chains)++];
        c.gd = gd;
        c.count = 1;
        c.sources[0] = src;
    };

    auto scan_dir = [&](const char *path) {
        DIR d;
        if (f_opendir(&d, path) != FR_OK) return;
        FILINFO fi;
        for (;;) {
            if (f_readdir(&d, &fi) != FR_OK || fi.fname[0] == 0) break;
            if (fi.fattrib & AM_DIR) continue;
            const char *dot = std::strrchr(fi.fname, '.');
            if (!dot || strcasecmp(dot, ".img") != 0) continue;
            char fullpath[64];
            if (std::snprintf(fullpath, sizeof(fullpath), "%s/%s", path, fi.fname)
                    >= (int)sizeof(fullpath)) continue;
            PcvSource src{};
            const GameDescriptor *gd = nullptr;
            if (probe_img(fullpath, &src, &gd)) {
                insert_into_chain(gd, src);
            }
        }
        f_closedir(&d);
    };

    scan_dir("/scumm");
    DIR d;
    if (f_opendir(&d, "/scumm") == FR_OK) {
        FILINFO fi;
        char sub[280];
        for (;;) {
            if (f_readdir(&d, &fi) != FR_OK || fi.fname[0] == 0) break;
            if (!(fi.fattrib & AM_DIR)) continue;
            if (fi.fname[0] == '.') continue;
            std::snprintf(sub, sizeof(sub), "/scumm/%s", fi.fname);
            scan_dir(sub);
        }
        f_closedir(&d);
    }

    for (int c = 0; c < *n_chains; ++c) {
        PcvChain &ch = chains[c];
        for (int i = 1; i < ch.count; ++i) {
            PcvSource tmp = ch.sources[i];
            int j = i;
            while (j > 0 && ch.sources[j-1].chain_letter > tmp.chain_letter) {
                ch.sources[j] = ch.sources[j-1];
                --j;
            }
            ch.sources[j] = tmp;
        }
    }
    return *n_chains > 0;
}

// ===========================================================================
// Stage B raw .img reading + outer-FAT manipulation
// ===========================================================================
//
// All three of these helpers (OuterFatCache, RawImgReader, the dir-
// entry clear) talk directly to the block device via disk_read/
// disk_write, bypassing FatFs.  They coordinate with FatFs's single
// cached window (fs->win / fs->winsect / fs->wflag) so reads/writes
// stay coherent when interleaved with f_write on the output file.

static constexpr DWORD kInvalidSec = (DWORD)-1;

// If FatFs has cached the given sector and it's dirty, push it to disk
// so a subsequent disk_read of that sector sees the latest bytes.
static void fatfs_sync_if_dirty(FATFS *fs, DWORD sec) {
    if (fs->winsect == sec && fs->wflag) {
        disk_write(fs->pdrv, fs->win, sec, 1);
        fs->wflag = 0;
    }
}

// If FatFs has cached the given sector, invalidate it so the next
// move_window re-reads from disk.  Call after we disk_write to that
// sector ourselves.
static void fatfs_invalidate_window(FATFS *fs, DWORD sec) {
    if (fs->winsect == sec) {
        fs->wflag = 0;
        fs->winsect = (LBA_t)kInvalidSec;
    }
}

// Batched cache for one FAT16 sector.  Reads through disk_read,
// writes through disk_write (to every FAT copy on the volume),
// coordinated with FatFs's win cache.
class OuterFatCache {
public:
    OuterFatCache(FATFS *fs)
        : _fs(fs), _sector(kInvalidSec), _dirty(false) {}

    ~OuterFatCache() { flush(); }

    DWORD get(DWORD cluster) {
        DWORD off = cluster * 2;
        DWORD sec = _fs->fatbase + off / 512;
        DWORD bof = off % 512;
        if (!load(sec)) return 0xFFFF;
        return (DWORD)(_buf[bof] | (_buf[bof + 1] << 8));
    }

    void set(DWORD cluster, DWORD value) {
        DWORD off = cluster * 2;
        DWORD sec = _fs->fatbase + off / 512;
        DWORD bof = off % 512;
        if (!load(sec)) return;
        _buf[bof]     = (uint8_t)(value & 0xFF);
        _buf[bof + 1] = (uint8_t)((value >> 8) & 0xFF);
        _dirty = true;
    }

    bool flush() {
        if (!_dirty || _sector == kInvalidSec) { _dirty = false; return true; }
        if (disk_write(_fs->pdrv, _buf, _sector, 1) != RES_OK) {
            _dirty = false;
            return false;
        }
        for (BYTE i = 1; i < _fs->n_fats; ++i) {
            DWORD s = _sector + _fs->fsize * i;
            disk_write(_fs->pdrv, _buf, s, 1);
        }
        _dirty = false;
        // Anything FatFs had cached for our FAT copies is now stale.
        for (BYTE i = 0; i < _fs->n_fats; ++i) {
            fatfs_invalidate_window(_fs, _sector + _fs->fsize * i);
        }
        return true;
    }

    // Flush + drop our cache.  Call before any FatFs op that might
    // touch the FAT (e.g. an output f_write that allocates a cluster).
    void before_fatfs_op() {
        flush();
        _sector = kInvalidSec;
    }

private:
    FATFS *_fs;
    DWORD  _sector;
    bool   _dirty;
    uint8_t _buf[512];

    bool load(DWORD sec) {
        if (_sector == sec) return true;
        flush();
        fatfs_sync_if_dirty(_fs, sec);
        if (disk_read(_fs->pdrv, _buf, sec, 1) != RES_OK) {
            _sector = kInvalidSec;
            return false;
        }
        _sector = sec;
        return true;
    }
};

// Sequential .img byte reader.  Walks the outer FAT16 chain one
// cluster at a time, frees each consumed outer cluster immediately,
// and reads the next via disk_read directly.  Bypasses FatFs entirely
// for the data — coordinates with its window cache as needed.
class RawImgReader {
public:
    RawImgReader()
        : _fs(nullptr), _fc(nullptr), _cur_cluster(0), _cluster_loaded(false),
          _byte_in_cluster(0), _csize_bytes(0), _img_size(0),
          _byte_pos(0), _eof(false) {}

    bool open(const PcvSource &src, FATFS *fs, OuterFatCache *fc) {
        _fs = fs;
        _fc = fc;
        _csize_bytes = (uint32_t)_fs->csize * 512u;
        _cur_cluster = src.outer_first_cluster;
        _byte_in_cluster = 0;
        _cluster_loaded = false;
        _img_size = src.outer_size_bytes;
        _byte_pos = 0;
        _eof = false;
        if (_cur_cluster < 2 || _cur_cluster >= 0xFFF8) {
            _eof = true;
            return false;
        }
        return true;
    }

    uint32_t read(uint8_t *out, uint32_t n) {
        uint32_t got = 0;
        while (got < n && !_eof) {
            if (_byte_in_cluster >= _csize_bytes) {
                if (!advance_outer()) break;
            }
            if (!ensure_loaded()) break;
            uint32_t cluster_remaining = _csize_bytes - _byte_in_cluster;
            uint32_t take = (n - got < cluster_remaining)
                          ? (n - got) : cluster_remaining;
            std::memcpy(out + got, _cluster_buf + _byte_in_cluster, take);
            _byte_in_cluster += take;
            _byte_pos += take;
            got += take;
        }
        return got;
    }

    bool skip(uint32_t n) {
        while (n > 0 && !_eof) {
            if (_byte_in_cluster >= _csize_bytes) {
                if (!advance_outer()) return false;
            }
            uint32_t cluster_remaining = _csize_bytes - _byte_in_cluster;
            uint32_t take = (n < cluster_remaining) ? n : cluster_remaining;
            _byte_in_cluster += take;
            _byte_pos += take;
            n -= take;
        }
        return n == 0;
    }

    // Free any outer clusters still in the .img's chain after the
    // current one (we've stopped reading partway through the file).
    void free_remaining() {
        if (_eof) return;
        // The current cluster is still allocated and partially read.
        DWORD c = _cur_cluster;
        while (c >= 2 && c < 0xFFF8) {
            DWORD next = _fc->get(c);
            _fc->set(c, 0);
            c = next;
        }
        _eof = true;
    }

private:
    FATFS *_fs;
    OuterFatCache *_fc;
    DWORD _cur_cluster;
    bool  _cluster_loaded;
    uint32_t _byte_in_cluster;
    uint32_t _csize_bytes;
    uint32_t _img_size;
    uint32_t _byte_pos;
    bool _eof;
    uint8_t _cluster_buf[1024];   // matches typical ThumbyOne csize

    bool ensure_loaded() {
        if (_cluster_loaded) return true;
        if (_cur_cluster < 2 || _cur_cluster >= 0xFFF8) {
            _eof = true;
            return false;
        }
        if (_csize_bytes > sizeof(_cluster_buf)) {
            tsb::platform::log("[pcv] csize %u too large\n",
                               (unsigned)_csize_bytes);
            _eof = true;
            return false;
        }
        DWORD sec_lba = _fs->database + (_cur_cluster - 2) * _fs->csize;
        for (BYTE i = 0; i < _fs->csize; ++i) {
            fatfs_sync_if_dirty(_fs, sec_lba + i);
        }
        if (disk_read(_fs->pdrv, _cluster_buf, sec_lba, _fs->csize) != RES_OK) {
            _eof = true;
            return false;
        }
        _cluster_loaded = true;
        return true;
    }

    // Free the current outer cluster and walk to the next one in the
    // chain.  The OuterFatCache batches the FAT writes per-sector.
    bool advance_outer() {
        DWORD next = _fc->get(_cur_cluster);
        _fc->set(_cur_cluster, 0);
        _cur_cluster = next;
        _byte_in_cluster = 0;
        _cluster_loaded = false;
        if (_cur_cluster < 2 || _cur_cluster >= 0xFFF8) {
            _eof = true;
            return false;
        }
        return true;
    }
};

// Find a file by 8.3 name inside a directory whose first cluster is
// `parent_first_cluster`, and overwrite the first byte of its dir
// entry with 0xE5.  Coordinates with FatFs's win cache.
static bool clear_dir_entry(FATFS *fs, DWORD parent_first_cluster,
                            const char *name11) {
    if (parent_first_cluster < 2 || parent_first_cluster >= 0xFFF8) return false;
    uint8_t buf[512];
    DWORD cluster = parent_first_cluster;
    while (cluster >= 2 && cluster < 0xFFF8) {
        DWORD sec_lba = fs->database + (cluster - 2) * fs->csize;
        for (BYTE sec_i = 0; sec_i < fs->csize; ++sec_i) {
            DWORD sec = sec_lba + sec_i;
            fatfs_sync_if_dirty(fs, sec);
            if (disk_read(fs->pdrv, buf, sec, 1) != RES_OK) return false;
            for (uint32_t e = 0; e < 16; ++e) {
                uint8_t *p = buf + e * 32;
                if (p[0] == 0) return false;        // end-of-dir, no match
                if (p[0] == 0xE5) continue;
                if ((p[11] & 0x0F) == 0x0F) continue;  // LFN entry
                if (std::memcmp(p, name11, 11) == 0) {
                    p[0] = 0xE5;
                    if (disk_write(fs->pdrv, buf, sec, 1) != RES_OK) return false;
                    fatfs_invalidate_window(fs, sec);
                    return true;
                }
            }
        }
        // Walk to next cluster of the directory via FAT16.
        DWORD off = cluster * 2;
        DWORD fat_sec = fs->fatbase + off / 512;
        DWORD fat_off = off % 512;
        fatfs_sync_if_dirty(fs, fat_sec);
        uint8_t fbuf[512];
        if (disk_read(fs->pdrv, fbuf, fat_sec, 1) != RES_OK) return false;
        cluster = (DWORD)(fbuf[fat_off] | (fbuf[fat_off + 1] << 8));
    }
    return false;
}

// ===========================================================================
// PcvStream — bytes-of-the-PCV-chain stream on top of RawImgReader.
// ===========================================================================
//
// One RawImgReader is active at a time (one .img).  When that .img's
// PCV is drained, we finalize the reader (free remaining outer
// clusters + clear the .img's dir entry) and open the next source.

class PcvStream {
public:
    bool init(const PcvChain &ch, FATFS *fs, OuterFatCache *fc);

    uint32_t read(uint8_t *out, uint32_t n);
    bool     skip(uint32_t n);
    bool     done() const { return _eof; }
    void     close();

private:
    PcvChain        _ch;
    int             _idx;
    FATFS          *_fs;
    OuterFatCache  *_fc;
    RawImgReader    _reader;
    bool            _reader_open;
    uint32_t        _bytes_left_in_pcv;
    bool            _eof;

    bool advance_to_next_source();
    bool ensure_byte_available();
};

bool PcvStream::init(const PcvChain &ch, FATFS *fs, OuterFatCache *fc) {
    _ch = ch;
    _fs = fs;
    _fc = fc;
    _idx = -1;
    _reader_open = false;
    _eof = false;
    return advance_to_next_source();
}

void PcvStream::close() {
    if (_reader_open) {
        _reader.free_remaining();
        _reader_open = false;
    }
}

bool PcvStream::advance_to_next_source() {
    if (_reader_open) {
        // We've finished this PCV — but the .img might still have
        // outer clusters past the PCV's logical end.  Free those too
        // before retiring the .img.
        _reader.free_remaining();
        // Now manually clear /scumm/.../<file>.img's dir entry.
        const PcvSource &prev = _ch.sources[_idx];
        DIR pd;
        if (f_opendir(&pd, prev.parent_dir) == FR_OK) {
            DWORD parent_sclust = pd.obj.sclust;
            f_closedir(&pd);
            // Make sure all our pending outer-FAT writes are on disk
            // before we touch the directory cluster (FatFs may share
            // its win cache between FAT and dir sectors).
            _fc->before_fatfs_op();
            if (!clear_dir_entry(_fs, parent_sclust, prev.basename_83)) {
                tsb::platform::log("[pcv] dir clear fail %s\n", prev.img_path);
            }
        }
        _reader_open = false;
    }

    _idx++;
    if (_idx >= _ch.count) { _eof = true; return false; }
    const PcvSource &s = _ch.sources[_idx];

    if (!_reader.open(s, _fs, _fc)) {
        tsb::platform::log("[pcv] raw open fail %s\n", s.img_path);
        _eof = true;
        return false;
    }
    _reader_open = true;

    // Seek to the PCV's start byte inside the .img (skipping BPB +
    // FAT + root + any earlier files).  RawImgReader::skip frees the
    // outer clusters we walk past.
    uint32_t pcv_start = (uint32_t)s.geom.data_first_sec * 512u
                       + (uint32_t)(s.inner_start_cluster - 2)
                         * s.geom.sectors_per_cluster * 512u;
    if (!_reader.skip(pcv_start)) {
        tsb::platform::log("[pcv] seek PCV start fail %s\n", s.img_path);
        _eof = true;
        return false;
    }

    _bytes_left_in_pcv = s.inner_size;

    // Continuation PCVs start with an 8-byte LFG!+size wrapper that
    // the FILE-chunk parser doesn't expect — eat it transparently.
    if (_idx > 0) {
        if (!skip(8)) return false;
    }
    return true;
}

bool PcvStream::ensure_byte_available() {
    while (!_eof) {
        if (_bytes_left_in_pcv == 0) {
            if (!advance_to_next_source()) return false;
            continue;
        }
        return true;
    }
    return false;
}

uint32_t PcvStream::read(uint8_t *out, uint32_t n) {
    uint32_t got = 0;
    while (got < n) {
        if (!ensure_byte_available()) break;
        uint32_t want = n - got;
        if (want > _bytes_left_in_pcv) want = _bytes_left_in_pcv;
        uint32_t r = _reader.read(out + got, want);
        if (r == 0) break;
        got += r;
        _bytes_left_in_pcv -= r;
    }
    return got;
}

bool PcvStream::skip(uint32_t n) {
    while (n > 0) {
        if (!ensure_byte_available()) return false;
        uint32_t take = (n < _bytes_left_in_pcv) ? n : _bytes_left_in_pcv;
        if (!_reader.skip(take)) return false;
        _bytes_left_in_pcv -= take;
        n -= take;
    }
    return true;
}

// ===========================================================================
// FILE chunk parser + DCL extractor (one file per chunk)
// ===========================================================================

struct FileChunkHeader {
    char     name[16];
    uint32_t chunk_size;
    uint32_t unpacked_size;
    uint8_t  stamp[3];
    uint16_t flags;
    uint32_t reserved;
};

static bool read_file_chunk_header(PcvStream &ps, FileChunkHeader *out) {
    uint8_t hdr[8];
    uint32_t got = ps.read(hdr, 8);
    if (got < 8) return false;
    if (std::memcmp(hdr, "FILE", 4) != 0) return false;
    out->chunk_size = (uint32_t)hdr[4]
                    | ((uint32_t)hdr[5] << 8)
                    | ((uint32_t)hdr[6] << 16)
                    | ((uint32_t)hdr[7] << 24);
    uint8_t meta[24];
    if (ps.read(meta, 24) != 24) return false;
    std::memcpy(out->name, meta, 11);
    out->name[11] = 0;
    std::memcpy(out->stamp, meta + 11, 3);
    out->unpacked_size = (uint32_t)meta[14]
                       | ((uint32_t)meta[15] << 8)
                       | ((uint32_t)meta[16] << 16)
                       | ((uint32_t)meta[17] << 24);
    out->flags         = (uint16_t)(meta[18] | (meta[19] << 8));
    out->reserved      = (uint32_t)meta[20]
                       | ((uint32_t)meta[21] << 8)
                       | ((uint32_t)meta[22] << 16)
                       | ((uint32_t)meta[23] << 24);
    return true;
}

static uint8_t descriptor_xor_for(const GameDescriptor &gd, const char *name) {
    if (!gd.files) return 0;
    size_t name_len = std::strlen(name);
    for (const GameFile *gf = gd.files; gf->name; ++gf) {
        if (strcasecmp(gf->name, name) == 0) return gf->xor_byte;
    }
    // PCV chunk-name field is fixed 11 bytes; filenames > 11 chars
    // are truncated.  Indy 4 ships `atlantis.000` / `atlantis.001`
    // (12 chars each, both truncate to `ATLANTIS.00`) so the exact
    // match above misses every chunk and the install drains
    // through every PCV without writing anything.  If the PCV name
    // is exactly 11 chars, accept it as a prefix match against any
    // descriptor entry whose full name starts with it.  Both
    // candidate entries share xor_byte for any game we ship today,
    // so it doesn't matter which descriptor wins the match — the
    // *output* filename is resolved separately in resolve_fs_name
    // below using unpacked_size.
    if (name_len == 11) {
        for (const GameFile *gf = gd.files; gf->name; ++gf) {
            if (std::strlen(gf->name) > name_len &&
                strncasecmp(gf->name, name, name_len) == 0) {
                return gf->xor_byte;
            }
        }
    }
    return 0xFF;
}

// Map a (possibly truncated) PCV chunk name to the full filename
// expected on the FAT.  Called from extract_one before opening the
// output FIL.  Game-specific table — for now only Indy 4's
// atlantis.000 / .001 pair needs disambiguation (truncated PCV
// name "ATLANTIS.00" collides; we tell them apart by unpacked
// size, which differs by ~3 orders of magnitude).
static const char *resolve_fs_name(const char *pcv_name,
                                   uint32_t unpacked_size,
                                   char *scratch, size_t scratch_size) {
    if (strcasecmp(pcv_name, "ATLANTIS.00") == 0) {
        // .001 = ~9.23 MB resource bundle, .000 = ~12 KB index.
        std::snprintf(scratch, scratch_size, "ATLANTIS.00%c",
                      unpacked_size > 1000000u ? '1' : '0');
        return scratch;
    }
    return pcv_name;
}

struct DclSrc {
    PcvStream *ps;
    uint32_t   left;
};
static int dcl_src_get(void *u) {
    DclSrc *s = (DclSrc *)u;
    if (s->left == 0) return -1;
    uint8_t b;
    if (s->ps->read(&b, 1) != 1) return -1;
    s->left--;
    return b;
}

struct DclSink {
    FIL           *out_fp;
    uint8_t        xor_byte;
    uint8_t        buf[512];
    uint32_t       pos;
    bool           err;
    uint32_t       total_size;
    uint32_t       written;
    int            last_pct;
    const char    *display_name;
    const char    *current_file;
    OuterFatCache *fc;
};
static bool flush_dcl_sink(DclSink *s) {
    if (s->pos == 0) return true;
    // f_write may allocate a new cluster for the output, modifying
    // FAT — make sure our outer-FAT cache is committed and dropped
    // first so we don't race FatFs's win.
    s->fc->before_fatfs_op();
    UINT bw = 0;
    if (f_write(s->out_fp, s->buf, s->pos, &bw) != FR_OK || bw != s->pos) {
        s->err = true;
        return false;
    }
    s->pos = 0;
    return true;
}
static bool dcl_sink_put(uint8_t b, void *u) {
    DclSink *s = (DclSink *)u;
    if (s->err) return false;
    s->buf[s->pos++] = (uint8_t)(b ^ s->xor_byte);
    s->written++;
    if (s->total_size > 0) {
        int pct = (int)((s->written * 100u) / s->total_size);
        if (pct != s->last_pct) {
            s->last_pct = pct;
            tsb::platform::preload_progress(s->display_name,
                                            s->current_file, pct);
        }
    }
    if (s->pos == sizeof(s->buf)) {
        return flush_dcl_sink(s);
    }
    return true;
}

static bool extract_one(PcvStream &ps, const FileChunkHeader &fh,
                        const GameDescriptor &gd, uint8_t *dcl_dict,
                        OuterFatCache *fc) {
    uint32_t dcl_left = fh.chunk_size - 24;

    uint8_t xor_byte = descriptor_xor_for(gd, fh.name);
    if (xor_byte == 0xFF) {
        if (!ps.skip(dcl_left)) return false;
        return true;
    }

    // Resolve the PCV's possibly-truncated 11-char chunk name to
    // the full FS filename our descriptor (and the SCUMM engine)
    // expects.  For most files the PCV name matches verbatim, but
    // Indy 4 ships its 12-char `atlantis.000` / `atlantis.001`
    // names truncated to `ATLANTIS.00` (indistinguishable by name
    // alone) — resolve_fs_name disambiguates by unpacked_size.
    char fs_name_buf[16];
    const char *fs_name = resolve_fs_name(fh.name, fh.unpacked_size,
                                          fs_name_buf, sizeof(fs_name_buf));

    char dst[64];
    if (std::snprintf(dst, sizeof(dst), "/scumm/%s/%s", gd.subdir, fs_name)
            >= (int)sizeof(dst)) return false;
    char sub[40];
    std::snprintf(sub, sizeof(sub), "/scumm/%s", gd.subdir);
    fc->before_fatfs_op();
    f_mkdir(sub);

    FIL outfp;
    fc->before_fatfs_op();
    if (f_open(&outfp, dst, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;

    tsb::platform::preload_progress(gd.display_name, fs_name, 0);

    DclSrc  src  = { &ps, dcl_left };
    DclSink sink = { &outfp, xor_byte, {}, 0, false,
                     fh.unpacked_size, 0, -1,
                     gd.display_name, fs_name, fc };

    bool ok = tsb::DclDecoder::decode(dcl_src_get, &src,
                                      dcl_sink_put, &sink,
                                      dcl_dict,
                                      fh.unpacked_size);
    if (!sink.err) flush_dcl_sink(&sink);
    fc->before_fatfs_op();
    f_close(&outfp);
    if (!ok || sink.err) {
        tsb::platform::log("[pcv] extract fail %s\n", fh.name);
        fc->before_fatfs_op();
        f_unlink(dst);
        return false;
    }
    if (src.left > 0) {
        if (!ps.skip(src.left)) return false;
    }
    return true;
}

// ===========================================================================
// Top-level: install one PCV chain
// ===========================================================================

static bool install_chain(const PcvChain &ch, FATFS *fs) {
    const GameDescriptor &gd = *ch.gd;
    tsb::platform::log("[pcv] install %s (%d disks)\n", gd.subdir, ch.count);

    OuterFatCache fc(fs);

    PcvStream ps;
    if (!ps.init(ch, fs, &fc)) return false;

    uint8_t hdr[28];
    if (ps.read(hdr, 28) != 28) { ps.close(); return false; }
    if (std::memcmp(hdr, "LFG!", 4) != 0) {
        tsb::platform::log("[pcv] bad LFG! magic\n");
        ps.close();
        return false;
    }

    uint8_t *dcl_dict = (uint8_t *)std::malloc(4096);
    if (!dcl_dict) { ps.close(); return false; }

    bool ok = true;
    FileChunkHeader fh;
    while (read_file_chunk_header(ps, &fh)) {
        if (!extract_one(ps, fh, gd, dcl_dict, &fc)) { ok = false; break; }
    }

    std::free(dcl_dict);
    ps.close();
    fc.flush();
    return ok;
}

}  // anonymous

// ===========================================================================
// Public entry point
// ===========================================================================

bool install_pcv_imgs() {
    FATFS *fs = tsb::platform_pico::get_fatfs();
    if (!fs) return false;

    PcvChain chains[4];
    int n = 0;
    if (!discover_chains(chains, 4, &n)) return false;

    bool any = false;
    for (int i = 0; i < n; ++i) {
        if (install_chain(chains[i], fs)) any = true;
    }
    return any;
}

}  // namespace preload
}  // namespace tsb
