// PCV install pipeline — Stage A.  See pcv_install.h for the
// design / scope decisions.  Algorithm correctness was verified
// against host-side Python + standalone C++ reference build that
// MD5-matches every file in MI1's PCV chain (see commit 69c644b).

#include "pcv_install.h"

#include "dcl.h"
#include "game_table.h"
#include "platform.h"

extern "C" {
#include "ff.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace tsb {
namespace preload {

namespace {

// ===========================================================================
// FAT12 helpers — operating on a single outer .img file via FatFs f_read.
// ===========================================================================
//
// We use FatFs to read raw sectors of each .img (treating it as just a
// byte blob).  Inside, the .img is FAT12-formatted 1.44 MB or 720 KB
// DOS floppy — we walk its BPB / FAT / root manually.

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

// FAT12 cluster chain step.  Returns next cluster, or 0xFFF on
// EOC/error (caller treats >= 0xFF8 as EOC).
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

// Walk the .img's root dir.  For each non-deleted file entry, invoke
// cb(name11, start_cluster, size); cb returns false to stop early.
template <typename CB>
static bool walk_img_root(FIL *fp, const ImgGeometry &g, CB cb) {
    const uint32_t root_secs = (g.root_entries * 32u + 511u) / 512u;
    uint8_t buf[512];
    for (uint32_t rs = 0; rs < root_secs; ++rs) {
        if (!read_img_sector(fp, g.root_first_sec + rs, buf)) return false;
        for (uint32_t e = 0; e < 16; ++e) {       // 16 entries per 512 B sec
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

// ===========================================================================
// PCV chain discovery
// ===========================================================================
//
// Each PCV inside a .img has a fixed-pattern 11-byte name:
//   MI1 :   "PCV10__?MI1" (? = chain letter A..C)
//   MI2 :   "PCV10__?MI2" (? = A..E)
//   Indy4:  "PCVIQA_?ND4" (? = A..F)
//
// We match by prefix to figure out which game's chain a particular
// PCV belongs to, then the chain letter at byte 8 to order them.

struct PcvPattern {
    const char *prefix;      // 8 chars to match in inner_name[0..7]
    const char *suffix;      // 3 chars to match in inner_name[8..10]
    int         letter_off;  // = 7 (chain letter is byte 7 inside the 11)
};

static const PcvPattern kPatternMI1   = { "PCV10__", "MI1", 7 };
static const PcvPattern kPatternMI2   = { "PCV10__", "MI2", 7 };
static const PcvPattern kPatternIndy4 = { "PCVIQA_", "ND4", 7 };

// Returns descriptor index for the game whose pattern matches this
// 11-byte inner filename, or -1.
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

// One PCV source identified inside one .img.
struct PcvSource {
    char     img_path[64];
    char     inner_name[12];
    char     chain_letter;
    uint16_t inner_start_cluster;
    uint32_t inner_size;
    ImgGeometry geom;
};

struct PcvChain {
    const GameDescriptor *gd;
    PcvSource sources[8];
    int       count;
};

// Inspect one .img: try to identify a single PCV file inside.
// Returns true on success and fills `out` with the source info.
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
        if (!gd) return true;   // not a PCV — keep scanning
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
    f_close(&fp);
    return found;
}

// Scan /scumm/ root + one level of subdirs for .img files; group by
// game.  Returns true if any chain was found.
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
    // Also peek one level of subdir
    DIR d;
    if (f_opendir(&d, "/scumm") == FR_OK) {
        FILINFO fi;
        char sub[280];   // /scumm/ + FF_LFN_BUF_LEN(255) + NUL
        for (;;) {
            if (f_readdir(&d, &fi) != FR_OK || fi.fname[0] == 0) break;
            if (!(fi.fattrib & AM_DIR)) continue;
            if (fi.fname[0] == '.') continue;
            std::snprintf(sub, sizeof(sub), "/scumm/%s", fi.fname);
            scan_dir(sub);
        }
        f_closedir(&d);
    }

    // Sort each chain by letter (insertion sort — at most 8 entries)
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
// PCV byte stream — pulls bytes from the embedded PCV file in each
// .img in sequence, transparently crossing PCV boundaries.  The
// caller asks for individual bytes (or skips ahead by N), and we
// walk the inner FAT12 chain of each PCV.
//
// At PCV boundary crossings, the LFG! header (8 bytes) on the
// continuation PCVs is automatically skipped.
//
// For Stage A: when a PCV (and therefore its .img) is fully drained,
// we close the .img's FIL, f_unlink it, and open the next .img.
// Stage B will add incremental cluster freeing inside this loop.
// ===========================================================================

class PcvStream {
public:
    bool init(const PcvChain &ch);

    // Read up to n bytes into out; returns actual bytes read (0 at EOF).
    uint32_t read(uint8_t *out, uint32_t n);

    // Skip-forward.
    bool skip(uint32_t n);

    // True once every PCV in the chain has been fully consumed.
    bool done() const { return _eof; }

    // For the FILE-chunk parser to know where it is.
    uint32_t global_pos() const { return _global_pos; }

    void close();

private:
    PcvChain     _ch;
    int          _idx;             // current source index
    FIL          _fp;               // current .img
    bool         _fp_open;
    ImgFatCache  _fat_cache;

    uint16_t     _cur_cluster;     // current inner cluster of PCV
    uint32_t     _bytes_left_in_cluster;
    uint32_t     _bytes_left_in_pcv;
    uint32_t     _global_pos;      // logical byte position across whole chain

    uint8_t      _sec_buf[512];
    int32_t      _sec_buf_sector;  // which absolute outer sector is buffered
    uint32_t     _byte_in_sec;     // 0..511 — next byte to consume from _sec_buf
    bool         _sec_buf_valid;

    bool         _eof;

    bool advance_to_next_source();
    bool ensure_byte_available();
    int  next_byte_in_cluster();   // returns -1 on cluster exhausted
    bool load_next_sector();
};

bool PcvStream::init(const PcvChain &ch) {
    _ch = ch;
    _idx = -1;
    _fp_open = false;
    _eof = false;
    _sec_buf_valid = false;
    _sec_buf_sector = -1;
    _byte_in_sec = 0;
    _global_pos = 0;
    return advance_to_next_source();
}

void PcvStream::close() {
    if (_fp_open) {
        f_close(&_fp);
        _fp_open = false;
    }
}

bool PcvStream::advance_to_next_source() {
    if (_fp_open) {
        f_close(&_fp);
        _fp_open = false;
        // Stage A: unlink the .img we just finished with.
        f_unlink(_ch.sources[_idx].img_path);
    }
    _idx++;
    if (_idx >= _ch.count) { _eof = true; return false; }
    const PcvSource &s = _ch.sources[_idx];
    if (f_open(&_fp, s.img_path, FA_READ) != FR_OK) {
        _eof = true; return false;
    }
    _fp_open = true;
    _fat_cache.sector = -1;
    _cur_cluster = s.inner_start_cluster;
    _bytes_left_in_pcv = s.inner_size;
    _bytes_left_in_cluster = (uint32_t)s.geom.sectors_per_cluster * 512u;
    _sec_buf_valid = false;
    _byte_in_sec = 0;
    // For all PCVs after the first in the chain, the first 8 bytes
    // are an LFG! + size wrapper that the parser doesn't want — skip
    // them silently.  The first PCV's full header (LFG! + size +
    // next_pcv_name + count + total) is parsed by the caller, so we
    // leave it intact.
    if (_idx > 0) {
        skip(8);
    }
    return true;
}

bool PcvStream::load_next_sector() {
    if (_cur_cluster < 2 || _cur_cluster >= 0xFF8) return false;
    const PcvSource &s = _ch.sources[_idx];
    // Which absolute outer-img sector are we at?
    uint32_t cluster_first_sec = s.geom.data_first_sec
                               + (uint32_t)(_cur_cluster - 2)
                                 * s.geom.sectors_per_cluster;
    uint32_t cluster_bytes_total = (uint32_t)s.geom.sectors_per_cluster * 512u;
    uint32_t bytes_consumed_in_cluster = cluster_bytes_total - _bytes_left_in_cluster;
    uint32_t sec_in_cluster = bytes_consumed_in_cluster / 512u;
    uint32_t abs_sec = cluster_first_sec + sec_in_cluster;
    if (!read_img_sector(&_fp, abs_sec, _sec_buf)) return false;
    _sec_buf_sector = (int32_t)abs_sec;
    _byte_in_sec    = bytes_consumed_in_cluster % 512u;
    _sec_buf_valid  = true;
    return true;
}

int PcvStream::next_byte_in_cluster() {
    if (_bytes_left_in_cluster == 0) return -1;
    if (!_sec_buf_valid || _byte_in_sec >= 512u) {
        if (!load_next_sector()) return -1;
    }
    uint8_t b = _sec_buf[_byte_in_sec++];
    _bytes_left_in_cluster--;
    _bytes_left_in_pcv--;
    if (_byte_in_sec >= 512u) _sec_buf_valid = false;
    return b;
}

bool PcvStream::ensure_byte_available() {
    if (_eof) return false;
    while (_bytes_left_in_cluster == 0) {
        if (_bytes_left_in_pcv == 0) {
            if (!advance_to_next_source()) return false;
            continue;
        }
        // Advance to next inner cluster
        const PcvSource &s = _ch.sources[_idx];
        uint16_t next_cl = fat12_next(&_fp, s.geom, _fat_cache, _cur_cluster);
        if (next_cl < 2 || next_cl >= 0xFF8) {
            // Inner chain ended unexpectedly — try moving on
            if (!advance_to_next_source()) return false;
            continue;
        }
        _cur_cluster = next_cl;
        _bytes_left_in_cluster = (uint32_t)s.geom.sectors_per_cluster * 512u;
        if (_bytes_left_in_cluster > _bytes_left_in_pcv)
            _bytes_left_in_cluster = _bytes_left_in_pcv;
        _sec_buf_valid = false;
    }
    return true;
}

uint32_t PcvStream::read(uint8_t *out, uint32_t n) {
    uint32_t got = 0;
    while (got < n) {
        if (!ensure_byte_available()) break;
        int b = next_byte_in_cluster();
        if (b < 0) break;
        out[got++] = (uint8_t)b;
        _global_pos++;
    }
    return got;
}

bool PcvStream::skip(uint32_t n) {
    uint8_t scratch[64];
    while (n > 0) {
        uint32_t take = n < sizeof(scratch) ? n : sizeof(scratch);
        uint32_t got  = read(scratch, take);
        if (got == 0) return false;
        n -= got;
    }
    return true;
}

// ===========================================================================
// FILE chunk parser + DCL extractor (one file per chunk)
// ===========================================================================

struct FileChunkHeader {
    char     name[16];          // null-terminated, up to 11 chars
    uint32_t chunk_size;        // size of the chunk content (after the 8 header bytes)
    uint32_t unpacked_size;
    uint8_t  stamp[3];
    uint16_t flags;
    uint32_t reserved;
};

// Returns false at end-of-chain (no more FILE chunks).
static bool read_file_chunk_header(PcvStream &ps, FileChunkHeader *out) {
    uint8_t hdr[8];
    uint32_t got = ps.read(hdr, 8);
    if (got < 8) return false;
    if (std::memcmp(hdr, "FILE", 4) != 0) return false;
    out->chunk_size = (uint32_t)hdr[4]
                    | ((uint32_t)hdr[5] << 8)
                    | ((uint32_t)hdr[6] << 16)
                    | ((uint32_t)hdr[7] << 24);

    // Read 11-byte fixed filename field + 13 bytes of metadata
    uint8_t meta[24];
    if (ps.read(meta, 24) != 24) return false;
    std::memcpy(out->name, meta, 11);
    out->name[11] = 0;
    // Trim trailing residue past first NUL
    for (int i = 0; i < 11; ++i) {
        if (out->name[i] == 0) { out->name[i] = 0; break; }
    }
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

// Look up the descriptor's xor byte for a given inner filename (case-insensitive).
// Returns 0 if not in the descriptor (means: skip / extract plain).
static uint8_t descriptor_xor_for(const GameDescriptor &gd, const char *name) {
    if (!gd.files) return 0;
    for (const GameFile *gf = gd.files; gf->name; ++gf) {
        if (strcasecmp(gf->name, name) == 0) return gf->xor_byte;
    }
    return 0xFF;   // 0xFF sentinel = "file unknown" (we won't write it)
}

// Streaming source / sink contexts for DclDecoder.
struct DclSrc {
    PcvStream *ps;
    uint32_t   left;     // dcl_data byte budget within this FILE chunk
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
    FIL        *out_fp;
    uint8_t     xor_byte;
    uint8_t     buf[512];
    uint32_t    pos;
    bool        err;
    // Progress reporting state — DCL is slow enough on flash that
    // we need per-percent UI updates or the install looks frozen.
    uint32_t    total_size;
    uint32_t    written;
    int         last_pct;
    const char *display_name;
    const char *current_file;
};
static bool flush_dcl_sink(DclSink *s) {
    if (s->pos == 0) return true;
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
    // Throttle UI paints to one per percent — each paint is a
    // full-frame LCD blit + DMA wait (~80 ms), so painting per byte
    // would dominate install time.
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

// Decompress one FILE chunk's DCL stream into /scumm/<gd.subdir>/<name>.
// Skips files that aren't listed in the descriptor (e.g. MONKEY.EXE).
// Returns true on success.
static bool extract_one(PcvStream &ps, const FileChunkHeader &fh,
                        const GameDescriptor &gd, uint8_t *dcl_dict) {
    // The chunk_size covers everything after "FILE"+chunk_size header,
    // including the 24-byte inner header we just read.  DCL bytes
    // remaining = chunk_size - 24.
    uint32_t dcl_left = fh.chunk_size - 24;

    // Skip files we don't recognise
    uint8_t xor_byte = descriptor_xor_for(gd, fh.name);
    if (xor_byte == 0xFF) {
        // Not in descriptor — skip its DCL data entirely
        if (!ps.skip(dcl_left)) return false;
        return true;
    }

    // Open destination
    char dst[64];
    if (std::snprintf(dst, sizeof(dst), "/scumm/%s/%s", gd.subdir, fh.name)
            >= (int)sizeof(dst)) return false;
    // Ensure subdir
    char sub[40];
    std::snprintf(sub, sizeof(sub), "/scumm/%s", gd.subdir);
    f_mkdir(sub);   // ignore FR_EXIST

    FIL outfp;
    if (f_open(&outfp, dst, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;

    tsb::platform::preload_progress(gd.display_name, fh.name, 0);

    DclSrc  src  = { &ps, dcl_left };
    DclSink sink = { &outfp, xor_byte, {}, 0, false,
                     fh.unpacked_size, 0, -1,
                     gd.display_name, fh.name };

    bool ok = tsb::DclDecoder::decode(dcl_src_get, &src,
                                      dcl_sink_put, &sink,
                                      dcl_dict,
                                      fh.unpacked_size);
    if (!sink.err) flush_dcl_sink(&sink);
    f_close(&outfp);
    if (!ok || sink.err) {
        tsb::platform::log("[pcv] extract fail %s\n", fh.name);
        f_unlink(dst);
        return false;
    }
    // DCL may have left bytes unread inside the chunk (padding after EOS).
    // Skip up to the next FILE chunk.
    if (src.left > 0) {
        if (!ps.skip(src.left)) return false;
    }
    return true;
}

// ===========================================================================
// Top-level: install one PCV chain
// ===========================================================================

static bool install_chain(const PcvChain &ch) {
    const GameDescriptor &gd = *ch.gd;
    tsb::platform::log("[pcv] install %s (%d disks)\n", gd.subdir, ch.count);

    PcvStream ps;
    if (!ps.init(ch)) return false;

    // Parse PCV-A header — 8 bytes LFG! + size + 20 bytes (next_pcv_name [14],
    // count [2], total_size [4]).  We only need to skip them.
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
        if (!extract_one(ps, fh, gd, dcl_dict)) { ok = false; break; }
    }

    std::free(dcl_dict);
    ps.close();
    return ok;
}

}  // anonymous

// ===========================================================================
// Public entry point
// ===========================================================================

bool install_pcv_imgs() {
    PcvChain chains[4];
    int n = 0;
    if (!discover_chains(chains, 4, &n)) return false;

    bool any = false;
    for (int i = 0; i < n; ++i) {
        if (install_chain(chains[i])) any = true;
    }
    return any;
}

}  // namespace preload
}  // namespace tsb
