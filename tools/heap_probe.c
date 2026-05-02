/*
 * heap_probe.c — LD_PRELOAD malloc tracker for Phase 0 RAM accounting.
 *
 * Build: gcc -shared -fPIC -O2 -o heap_probe.so heap_probe.c -ldl -pthread
 * Use:   HEAP_PROBE_LOG=/tmp/heap.log LD_PRELOAD=./heap_probe.so ./scummvm ...
 *
 * Dumps a summary on exit. Send SIGUSR1 to dump mid-run.
 *
 * What it measures:
 *   - current outstanding bytes (live)
 *   - peak outstanding bytes
 *   - total allocations / frees
 *   - per-size-class histogram (bytes + counts)
 *
 * Bootstrap note: dlsym calls calloc, so we serve a small static arena until
 * the real allocator symbols resolve.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *(*malloc_fn)(size_t);
typedef void *(*calloc_fn)(size_t, size_t);
typedef void *(*realloc_fn)(void *, size_t);
typedef void  (*free_fn)(void *);

static malloc_fn  real_malloc  = NULL;
static calloc_fn  real_calloc  = NULL;
static realloc_fn real_realloc = NULL;
static free_fn    real_free    = NULL;

/* Bootstrap arena: dlsym calls calloc before we have real_calloc. */
#define BOOT_ARENA_BYTES (64 * 1024)
static unsigned char boot_arena[BOOT_ARENA_BYTES];
static _Atomic size_t boot_off = 0;
static int in_boot_arena(const void *p) {
    return (const unsigned char *)p >= boot_arena &&
           (const unsigned char *)p <  boot_arena + BOOT_ARENA_BYTES;
}
static void *boot_alloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    size_t off = atomic_fetch_add(&boot_off, n);
    if (off + n > BOOT_ARENA_BYTES) return NULL;
    return boot_arena + off;
}

/* Stats. Atomics so SDL audio thread doesn't corrupt counters. */
static _Atomic size_t live_bytes  = 0;
static _Atomic size_t peak_bytes  = 0;
static _Atomic size_t total_allocs = 0;
static _Atomic size_t total_frees  = 0;
static _Atomic size_t total_bytes_alloced = 0;

/* Per-IP tracking: simple open-addressing hash table of caller -> live bytes.
 * Header layout grows to 24 bytes: [size][magic][ip] */
#define IP_TABLE_BITS 16
#define IP_TABLE_SIZE (1 << IP_TABLE_BITS)
#define IP_TABLE_MASK (IP_TABLE_SIZE - 1)
typedef struct { _Atomic uintptr_t ip; _Atomic size_t live; _Atomic size_t total; _Atomic size_t allocs; } ip_slot_t;
static ip_slot_t ip_table[IP_TABLE_SIZE];
static inline uint32_t ip_hash(uintptr_t ip) {
    ip ^= ip >> 33; ip *= 0xff51afd7ed558ccdULL;
    ip ^= ip >> 33; ip *= 0xc4ceb9fe1a85ec53ULL;
    ip ^= ip >> 33;
    return (uint32_t)(ip & IP_TABLE_MASK);
}
static void ip_record(uintptr_t ip, ssize_t delta_live, ssize_t delta_total, ssize_t delta_allocs) {
    if (!ip) return;
    uint32_t h = ip_hash(ip);
    for (int probe = 0; probe < 64; probe++) {
        uint32_t idx = (h + probe) & IP_TABLE_MASK;
        uintptr_t cur = atomic_load(&ip_table[idx].ip);
        if (cur == ip) goto hit;
        if (cur == 0) {
            uintptr_t expected = 0;
            if (atomic_compare_exchange_strong(&ip_table[idx].ip, &expected, ip)) goto hit;
            if (expected == ip) goto hit;
        }
        continue;
hit:
        if (delta_live   > 0) atomic_fetch_add(&ip_table[idx].live,   (size_t)delta_live);
        else if (delta_live < 0) atomic_fetch_sub(&ip_table[idx].live, (size_t)(-delta_live));
        if (delta_total  > 0) atomic_fetch_add(&ip_table[idx].total,  (size_t)delta_total);
        if (delta_allocs > 0) atomic_fetch_add(&ip_table[idx].allocs, (size_t)delta_allocs);
        return;
    }
}

#define BUCKETS 12
static const size_t bucket_caps[BUCKETS] = {
    16, 64, 256, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304,
    16777216, SIZE_MAX
};
static const char *bucket_names[BUCKETS] = {
    "[0-16)", "[16-64)", "[64-256)", "[256-1k)", "[1k-4k)", "[4k-16k)",
    "[16k-64k)", "[64k-256k)", "[256k-1M)", "[1M-4M)", "[4M-16M)", "[16M+)"
};
static _Atomic size_t bucket_count[BUCKETS];
static _Atomic size_t bucket_bytes[BUCKETS];

/* 32-byte header so we can stash size + magic + caller IP. */
#define HDR_BYTES 32
#define HDR_MAGIC 0xC0FFEE5UL

static inline void  hdr_set(void *user_ptr, size_t sz, uintptr_t ip) {
    uint64_t *h = (uint64_t *)((unsigned char *)user_ptr - HDR_BYTES);
    h[0] = sz;
    h[1] = HDR_MAGIC;
    h[2] = (uint64_t)ip;
}
static inline size_t hdr_get_size(const void *user_ptr) {
    const uint64_t *h = (const uint64_t *)((const unsigned char *)user_ptr - HDR_BYTES);
    return (h[1] == HDR_MAGIC) ? h[0] : 0;
}
static inline uintptr_t hdr_get_ip(const void *user_ptr) {
    const uint64_t *h = (const uint64_t *)((const unsigned char *)user_ptr - HDR_BYTES);
    return (h[1] == HDR_MAGIC) ? (uintptr_t)h[2] : 0;
}

static void bump_live(size_t delta) {
    size_t now = atomic_fetch_add(&live_bytes, delta) + delta;
    size_t prev = atomic_load(&peak_bytes);
    while (now > prev && !atomic_compare_exchange_weak(&peak_bytes, &prev, now)) {}
}

static void bucket_add(size_t sz) {
    for (int i = 0; i < BUCKETS; i++) {
        if (sz < bucket_caps[i]) {
            atomic_fetch_add(&bucket_count[i], 1);
            atomic_fetch_add(&bucket_bytes[i], sz);
            return;
        }
    }
}

static void resolve_real(void) {
    if (real_malloc) return;
    real_malloc  = (malloc_fn)  dlsym(RTLD_NEXT, "malloc");
    real_calloc  = (calloc_fn)  dlsym(RTLD_NEXT, "calloc");
    real_realloc = (realloc_fn) dlsym(RTLD_NEXT, "realloc");
    real_free    = (free_fn)    dlsym(RTLD_NEXT, "free");
}

void *malloc(size_t sz) {
    uintptr_t ip = (uintptr_t)__builtin_return_address(0);
    if (!real_malloc) {
        resolve_real();
        if (!real_malloc) return boot_alloc(sz);
    }
    void *raw = real_malloc(sz + HDR_BYTES);
    if (!raw) return NULL;
    void *user = (unsigned char *)raw + HDR_BYTES;
    hdr_set(user, sz, ip);
    atomic_fetch_add(&total_allocs, 1);
    atomic_fetch_add(&total_bytes_alloced, sz);
    bucket_add(sz);
    bump_live(sz);
    ip_record(ip, (ssize_t)sz, (ssize_t)sz, 1);
    return user;
}

void *calloc(size_t n, size_t sz) {
    uintptr_t ip = (uintptr_t)__builtin_return_address(0);
    size_t total = n * sz;
    if (!real_calloc) {
        if (!real_malloc) resolve_real();
        if (!real_calloc) {
            void *p = boot_alloc(total);
            if (p) memset(p, 0, total);
            return p;
        }
    }
    void *raw = real_calloc(1, total + HDR_BYTES);
    if (!raw) return NULL;
    void *user = (unsigned char *)raw + HDR_BYTES;
    hdr_set(user, total, ip);
    atomic_fetch_add(&total_allocs, 1);
    atomic_fetch_add(&total_bytes_alloced, total);
    bucket_add(total);
    bump_live(total);
    ip_record(ip, (ssize_t)total, (ssize_t)total, 1);
    return user;
}

void *realloc(void *p, size_t sz) {
    uintptr_t ip = (uintptr_t)__builtin_return_address(0);
    if (!p) return malloc(sz);
    if (in_boot_arena(p)) {
        void *q = malloc(sz);
        if (q) memcpy(q, p, sz);
        return q;
    }
    if (!real_realloc) {
        resolve_real();
        if (!real_realloc) return NULL;
    }
    size_t old = hdr_get_size(p);
    uintptr_t old_ip = hdr_get_ip(p);
    void *raw_old = (unsigned char *)p - HDR_BYTES;
    void *raw_new = real_realloc(raw_old, sz + HDR_BYTES);
    if (!raw_new) return NULL;
    void *user = (unsigned char *)raw_new + HDR_BYTES;
    hdr_set(user, sz, ip);
    if (sz > old) bump_live(sz - old);
    else atomic_fetch_sub(&live_bytes, old - sz);
    bucket_add(sz);
    /* Move accounting from old IP to new IP. */
    if (old_ip) ip_record(old_ip, -(ssize_t)old, 0, 0);
    ip_record(ip, (ssize_t)sz, (ssize_t)(sz > old ? sz - old : 0), 1);
    return user;
}

void free(void *p) {
    if (!p) return;
    if (in_boot_arena(p)) return;
    if (!real_free) {
        resolve_real();
        if (!real_free) return;
    }
    size_t sz = hdr_get_size(p);
    uintptr_t ip = hdr_get_ip(p);
    if (sz) atomic_fetch_sub(&live_bytes, sz);
    atomic_fetch_add(&total_frees, 1);
    if (ip && sz) ip_record(ip, -(ssize_t)sz, 0, 0);
    real_free((unsigned char *)p - HDR_BYTES);
}

static void dump(void) {
    const char *path = getenv("HEAP_PROBE_LOG");
    FILE *f = (path && *path) ? fopen(path, "w") : stderr;
    if (!f) f = stderr;
    fprintf(f, "=== heap_probe summary ===\n");
    fprintf(f, "live now      : %12zu bytes (%.1f KB)\n",
            atomic_load(&live_bytes), atomic_load(&live_bytes) / 1024.0);
    fprintf(f, "peak          : %12zu bytes (%.1f KB)\n",
            atomic_load(&peak_bytes), atomic_load(&peak_bytes) / 1024.0);
    fprintf(f, "total allocs  : %12zu\n", atomic_load(&total_allocs));
    fprintf(f, "total frees   : %12zu\n", atomic_load(&total_frees));
    fprintf(f, "total alloced : %12zu bytes (%.1f MB lifetime)\n",
            atomic_load(&total_bytes_alloced),
            atomic_load(&total_bytes_alloced) / (1024.0 * 1024.0));
    fprintf(f, "\nsize class histogram:\n");
    fprintf(f, "%-12s %12s %12s\n", "bucket", "count", "bytes");
    for (int i = 0; i < BUCKETS; i++) {
        size_t c = atomic_load(&bucket_count[i]);
        size_t b = atomic_load(&bucket_bytes[i]);
        if (c || b) {
            fprintf(f, "%-12s %12zu %12zu (%.1f KB)\n",
                    bucket_names[i], c, b, b / 1024.0);
        }
    }
    /* Top IPs by live bytes. */
    fprintf(f, "\ntop 40 call sites by live bytes:\n");
    fprintf(f, "%-18s %12s %12s %10s\n", "ip", "live", "total", "allocs");
    /* Find top-N by simple selection scan. */
    enum { TOPN = 40 };
    uintptr_t top_ip[TOPN] = {0};
    size_t    top_live[TOPN] = {0};
    size_t    top_total[TOPN] = {0};
    size_t    top_allocs[TOPN] = {0};
    for (uint32_t i = 0; i < IP_TABLE_SIZE; i++) {
        uintptr_t ip = atomic_load(&ip_table[i].ip);
        size_t live  = atomic_load(&ip_table[i].live);
        if (!ip || !live) continue;
        size_t tot   = atomic_load(&ip_table[i].total);
        size_t cnt   = atomic_load(&ip_table[i].allocs);
        for (int j = 0; j < TOPN; j++) {
            if (live > top_live[j]) {
                for (int k = TOPN - 1; k > j; k--) {
                    top_ip[k] = top_ip[k-1]; top_live[k] = top_live[k-1];
                    top_total[k] = top_total[k-1]; top_allocs[k] = top_allocs[k-1];
                }
                top_ip[j] = ip; top_live[j] = live;
                top_total[j] = tot; top_allocs[j] = cnt;
                break;
            }
        }
    }
    fprintf(f, "%-12s %-12s %-10s  %s\n",
            "live", "total", "allocs", "callsite");
    for (int i = 0; i < TOPN; i++) {
        if (!top_ip[i]) break;
        Dl_info info;
        const char *sym = "?";
        const char *mod = "?";
        ptrdiff_t sym_off = 0;
        unsigned long file_off = 0;
        if (dladdr((void *)top_ip[i], &info)) {
            if (info.dli_sname) sym = info.dli_sname;
            if (info.dli_fname) mod = info.dli_fname;
            if (info.dli_saddr) sym_off = (char *)top_ip[i] - (char *)info.dli_saddr;
            if (info.dli_fbase) file_off = (unsigned long)((char *)top_ip[i] - (char *)info.dli_fbase);
        }
        const char *bn = strrchr(mod, '/');
        bn = bn ? bn + 1 : mod;
        fprintf(f, "%-12zu %-12zu %-10zu  %s+0x%lx  %s@0x%lx\n",
                top_live[i], top_total[i], top_allocs[i],
                sym, (unsigned long)sym_off, bn, file_off);
    }
    if (f != stderr) fclose(f);
}

static void *sampler_thread(void *arg) {
    (void)arg;
    int interval = 3;
    const char *iv = getenv("HEAP_PROBE_INTERVAL_SEC");
    if (iv && atoi(iv) > 0) interval = atoi(iv);
    while (1) {
        sleep(interval);
        dump();
    }
    return NULL;
}

__attribute__((constructor))
static void init(void) {
    resolve_real();
    if (getenv("HEAP_PROBE_LOG")) {
        pthread_t t;
        pthread_create(&t, NULL, sampler_thread, NULL);
        pthread_detach(t);
    }
    atexit(dump);
}
