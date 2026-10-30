// libarm64jit: the guest heap behind native malloc (malloc.cpp).
//
// Guest heap blocks must lie below 64 GB: arm64 iOS code packs pointers with flag bits above bit 36-47 (Swift and
// ObjC both do), and Darling's own malloc hands out 0x7f.. addresses. So this is a small native allocator over memory
// from reserve_aligned() (the guest-VM range [12 GB, 63 GB)):
// - small blocks (<= 32 KB): 40 size classes (16-byte steps to 128, then 4 per doubling), each class carving 1 MB
//   chunks; power-of-two classes are naturally aligned (blocks start at the 1 MB-aligned chunk base), which serves
//   memalign up to 4 KB. Per-thread free-list caches, refilled from / flushed to a global list per class.
// - large blocks: their own 16K-aligned mapping, sizes in a locked map.
// A chunk table (one byte per MB of the guest range) gives malloc_size in O(1). Pointers the heap does not own fall
// back to Darling's malloc (blocks that native code allocated and the guest frees).
// Speed 8: a scavenger thread (every 5 s) sorts each class's global free list and gives back (MADV_DONTNEED) the whole
// pages that free blocks cover; those blocks move to a side list kept in host memory (their headers would re-touch
// the pages), and refill hands them out after the free list. X86CODE_A64_GHEAP_SCAVENGE=0 turns it off.
#include "a64.h"
#include <cerrno>
#include <atomic>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>

namespace a64 {

u64 reserve_aligned(u64 size, u64 mask);

namespace {

constexpr u64 kLo = 0x300000000ull, kHi = 0xFC0000000ull, kChunk = 1ull << 20;
constexpr size_t kMaxSmall = 32768;
constexpr int kClasses = 40;
uint32_t g_class_size[kClasses + 1];
uint8_t g_class_of[kMaxSmall / 16 + 1];      // (size + 15) / 16 -> class
uint8_t g_chunk_class[(kHi - kLo) / kChunk];  // 0: not a small chunk; else class + 1

struct FreeBlock { FreeBlock *next; u64 magic; };
// Freed small blocks carry a marker in their second word: a second free of the same block is reported (with the
// guest backtrace) and ignored instead of corrupting the free lists.
constexpr u64 kFreeMagic = 0x5f2c8e1d0a7b4f63ull;
struct Global {
    std::mutex mu;
    FreeBlock *free = nullptr;
    u64 carve = 0, carve_end = 0;  // current chunk
    u64 chunks = 0, nfree = 0;     // stats (memlog): chunks carved, blocks on the global list
    std::vector<u64> released;     // free blocks whose pages were given back (no header written)
    u64 scavenged_at = 0;          // nfree after the last scavenge (skip a class until it grows)
} g_global[kClasses];
std::atomic<u64> g_released_bytes{0};

std::mutex g_large_mu;
std::unordered_map<u64, u64> g_large;  // address -> mapped size
u64 g_large_bytes;

struct Native {
    void (*free)(void *);
    size_t (*size)(const void *);
} d;

struct Cache {
    FreeBlock *head[kClasses] = {};
    uint32_t count[kClasses] = {};
    Cache *next = nullptr, **prev = nullptr;  // all thread caches (stats)
    Cache();
    ~Cache();
};
std::mutex g_caches_mu;
Cache *g_caches;
Cache::Cache() {
    std::lock_guard l(g_caches_mu);
    next = g_caches; prev = &g_caches;
    if (g_caches) g_caches->prev = &next;
    g_caches = this;
}
thread_local Cache t_cache;
void start_scavenger();

uint32_t limit_for(int c) {  // cached blocks per class: about 32 KB, at least 2, at most 256
    // (Speed 8: was 64 KB / at least 8, so the 13 classes of 8-32 KB held up to 1.5 MB per thread)
    uint32_t n = 32768 / g_class_size[c];
    return n < 2 ? 2 : n > 256 ? 256 : n;
}

void init_classes() {
    int c = 0;
    for (uint32_t s = 16; s <= 128; s += 16) g_class_size[c++] = s;
    for (uint32_t base = 128; base < kMaxSmall; base *= 2)
        for (int k = 1; k <= 4; k++) g_class_size[c++] = base + base / 4 * k;
    if (c != kClasses) die("gheap: %d classes", c);
    for (size_t q = 0, k = 0; q <= kMaxSmall / 16; q++) {
        while (g_class_size[k] < q * 16) k++;
        g_class_of[q] = (uint8_t)k;
    }
}
struct Init { Init() { init_classes(); } } g_init;

FreeBlock *refill(int c, uint32_t want, uint32_t *got) {
    Global &g = g_global[c];
    std::lock_guard l(g.mu);
    FreeBlock *head = nullptr; uint32_t n = 0;
    while (n < want && g.free) { FreeBlock *b = g.free; g.free = b->next; b->next = head; head = b; n++; g.nfree--; }
    while (n < want && !g.released.empty()) {
        auto *b = (FreeBlock *)g.released.back(); g.released.pop_back();
        g_released_bytes -= g_class_size[c];
        b->next = head; head = b; n++;
    }
    u64 sz = g_class_size[c];
    while (n < want) {
        if (g.carve + sz > g.carve_end) {
            u64 a = reserve_aligned(kChunk, kChunk - 1);
            if (!a || mprotect((void *)a, kChunk, PROT_READ | PROT_WRITE)) {
                logf("gheap: no chunk for class %d (reserve %#lx, errno %d)", c, (unsigned long)a, errno);
                break;
            }
            g_chunk_class[(a - kLo) / kChunk] = (uint8_t)(c + 1);
            g.carve = a; g.carve_end = a + kChunk; g.chunks++;
            start_scavenger();
        }
        auto *b = (FreeBlock *)g.carve; g.carve += sz;
        b->next = head; head = b; n++;
    }
    *got = n;
    return head;
}
void flush(int c, uint32_t keep) {
    Cache &tc = t_cache;
    FreeBlock *list = nullptr; uint32_t n = 0;
    while (tc.count[c] > keep) { FreeBlock *b = tc.head[c]; tc.head[c] = b->next; b->next = list; list = b; tc.count[c]--; n++; }
    if (!list) return;
    FreeBlock *tail = list; while (tail->next) tail = tail->next;
    Global &g = g_global[c];
    std::lock_guard l(g.mu);
    tail->next = g.free; g.free = list; g.nfree += n;
}
Cache::~Cache() {
    for (int c = 0; c < kClasses; c++) flush(c, 0);
    std::lock_guard l(g_caches_mu);
    *prev = next;
    if (next) next->prev = prev;
}

// Gives back the pages of class c's global free list that lie wholly inside runs of adjacent free blocks.
void scavenge(int c) {
    Global &g = g_global[c];
    const u64 sz = g_class_size[c];
    std::lock_guard l(g.mu);
    if (g.nfree * sz < (256u << 10) || g.nfree < g.scavenged_at + g.scavenged_at / 4) return;
    std::vector<u64> v; v.reserve(g.nfree);
    for (FreeBlock *b = g.free; b; b = b->next) v.push_back((u64)b);
    std::sort(v.begin(), v.end());
    FreeBlock *keep = nullptr; u64 nkeep = 0;
    auto put = [&](u64 a) { auto *b = (FreeBlock *)a; b->next = keep; keep = b; nkeep++; };
    for (size_t i = 0; i < v.size();) {
        size_t j = i + 1;
        while (j < v.size() && v[j] == v[j - 1] + sz) j++;
        const u64 s = v[i], e = v[j - 1] + sz, ps = (s + 0xfff) & ~0xfffull, pe = e & ~0xfffull;
        if (pe > ps) {
            madvise((void *)ps, pe - ps, MADV_DONTNEED);
            for (size_t k = i; k < j; k++) {
                if (v[k] + sz > ps && v[k] < pe) { g.released.push_back(v[k]); g_released_bytes += sz; }
                else put(v[k]);  // its header page stays: rewrite the list link below
            }
        } else {
            for (size_t k = i; k < j; k++) put(v[k]);
        }
        i = j;
    }
    // put() relinked in reverse address order: magic words are untouched, the links are rewritten
    g.free = keep; g.nfree = nkeep; g.scavenged_at = nkeep;
}
void start_scavenger() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (const char *e = getenv("X86CODE_A64_GHEAP_SCAVENGE"); e && *e == '0') return;
        std::thread([] { for (;;) { sleep(5); for (int c = 0; c < kClasses; c++) scavenge(c); } }).detach();
    });
}

inline int class_of_ptr(u64 p) {
    if (p < kLo || p >= kHi) return -1;
    return (int)g_chunk_class[(p - kLo) / kChunk] - 1;
}

void *small_alloc(int c) {
    Cache &tc = t_cache;
    FreeBlock *b = tc.head[c];
    if (__builtin_expect(!b, 0)) {
        uint32_t got;
        b = refill(c, limit_for(c) / 2 + 1, &got);
        if (!b) return nullptr;
        tc.head[c] = b; tc.count[c] = got;
    }
    tc.head[c] = b->next; tc.count[c]--;
    b->magic = 0;
    return b;
}
void *large_alloc(size_t size, size_t align) {
    u64 len = (size + 0x3fff) & ~0x3fffull;
    u64 a = reserve_aligned(len, align > 0x4000 ? align - 1 : 0);
    if (!a || mprotect((void *)a, len, PROT_READ | PROT_WRITE)) {
        logf("gheap: large allocation of %zu bytes (align %zu) failed (reserve %#lx, errno %d)", size, align, (unsigned long)a, errno);
        return nullptr;
    }
    std::lock_guard l(g_large_mu);
    g_large[a] = len; g_large_bytes += len;
    return (void *)a;
}

}  // namespace

void gheap_set_native(void (*nfree)(void *), size_t (*nsize)(const void *)) { d.free = nfree; d.size = nsize; }

size_t gh_size(const void *p) {
    int c = class_of_ptr((u64)p);
    if (c >= 0) return g_class_size[c];
    if ((u64)p >= kLo && (u64)p < kHi) {
        std::lock_guard l(g_large_mu);
        auto it = g_large.find((u64)p);
        if (it != g_large.end()) return it->second;
        return 0;
    }
    return p && d.size ? d.size(p) : 0;
}

void *gh_memalign(size_t align, size_t size) {
    if (!size) size = 1;
    if (align <= 16) {
        if (size <= kMaxSmall) return small_alloc(g_class_of[(size + 15) >> 4]);
        return large_alloc(size, 0);
    }
    if (align <= 4096 && size <= kMaxSmall) {
        size_t s = size > align ? size : align;
        size_t p2 = 1; while (p2 < s) p2 <<= 1;
        if (p2 <= kMaxSmall) return small_alloc(g_class_of[p2 >> 4]);
    }
    return large_alloc(size, align);
}
void *gh_malloc(size_t size) {
    if (__builtin_expect(size <= kMaxSmall, 1)) return small_alloc(g_class_of[(size + 15) >> 4]);
    return large_alloc(size, 0);
}
void *gh_calloc(size_t n, size_t s) {
    size_t t;
    if (__builtin_mul_overflow(n, s, &t)) return nullptr;
    void *p = gh_malloc(t);
    if (p) memset(p, 0, t);
    return p;
}
void gh_free(void *p) {
    if (!p) return;
    int c = class_of_ptr((u64)p);
    if (__builtin_expect(c >= 0, 1)) {
        Cache &tc = t_cache;
        auto *b = (FreeBlock *)p;
        if (__builtin_expect(b->magic == kFreeMagic, 0)) {
            static std::atomic<int> reported{0};
            if (reported++ < 20) {
                logf("gheap: double free of %p (class size %u)", p, g_class_size[c]);
                if (GuestThread *t = GuestThread::current()) guest_backtrace(*t);
            }
            return;
        }
        b->magic = kFreeMagic;
        b->next = tc.head[c]; tc.head[c] = b;
        if (++tc.count[c] > limit_for(c)) flush(c, limit_for(c) / 2);
        return;
    }
    if ((u64)p >= kLo && (u64)p < kHi) {
        u64 len = 0;
        {
            std::lock_guard l(g_large_mu);
            auto it = g_large.find((u64)p);
            if (it != g_large.end()) { len = it->second; g_large.erase(it); g_large_bytes -= len; }
        }
        if (len) munmap(p, len);
        else logf("gheap: free(%p): not a heap block (ignored)", p);
        return;
    }
    if (d.size && d.size(p)) { d.free(p); return; }
    static int warned;
    if (!warned++) logf("gheap: free(%p): not a heap block (ignored)", p);
}
void *gh_realloc(void *p, size_t size) {
    if (!p) return gh_malloc(size);
    size_t old = gh_size(p);
    if (old >= size && (old <= kMaxSmall || size > kMaxSmall) && size + 4096 > old / 2) return p;
    void *q = gh_malloc(size);
    if (!q) return nullptr;
    memcpy(q, p, old < size ? old : size);
    gh_free(p);
    return q;
}
// memlog line: small chunks carved / blocks on the global free lists (thread caches not counted) / large blocks, MB
void gheap_stats() {
    u64 chunks = 0, freeb = 0; std::string top;
    std::vector<std::pair<u64, int>> v;
    for (int c = 0; c < kClasses; c++) {
        std::lock_guard l(g_global[c].mu);
        chunks += g_global[c].chunks; freeb += g_global[c].nfree * g_class_size[c];
        if (g_global[c].chunks) v.push_back({g_global[c].chunks, c});
    }
    std::sort(v.rbegin(), v.rend());
    for (size_t k = 0; k < v.size() && k < 8; k++) {
        const Global &g = g_global[v[k].second];
        char b[64]; snprintf(b, sizeof b, " %u:%lu/%.1f", g_class_size[v[k].second], (unsigned long)g.chunks, g.nfree * g_class_size[v[k].second] / 1048576.0);
        top += b;
    }
    u64 lb, ln, cached = 0; int nc = 0;
    { std::lock_guard l(g_large_mu); lb = g_large_bytes; ln = g_large.size(); }
    {
        std::lock_guard l(g_caches_mu);
        for (Cache *k = g_caches; k; k = k->next, nc++)
            for (int c = 0; c < kClasses; c++) cached += (u64)k->count[c] * g_class_size[c];
    }
    logf("memlog: gheap small %lu MB carved, %.1f MB on free lists (%.1f MB released), %.1f MB in %d thread caches; "
         "large %.1f MB in %lu; classes size:MB/freeMB%s", (unsigned long)chunks, freeb / 1048576.0,
         g_released_bytes.load() / 1048576.0, cached / 1048576.0, nc, lb / 1048576.0, (unsigned long)ln, top.c_str());
}
size_t gh_good_size(size_t size) {
    if (size <= kMaxSmall) return g_class_size[g_class_of[(size + 15) >> 4]];
    return (size + 0x3fff) & ~0x3fffull;
}

}  // namespace a64
