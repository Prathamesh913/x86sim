// libarm64jit: objc_msgSend fast paths.
//
// Two modes (in preference order):
//   1. Inline x86 probe in dynarmic (default): when a block at _objc_msgSend is translated,
//      EmitInlineObjCMsgSend emits the cache lookup in x86 and FastDispatches to the IMP.
//      No CallSVC, no HostCall spill. X86CODE_A64_INLINE_OBJC=0 disables.
//   2. Legacy svc stub (X86CODE_A64_NATIVE_OBJC=1): patch entry to svc; host C++ probe;
//      SetPC(IMP). Kept for comparison; slower than guest A64 (~1.8×) because of the boundary.
//
// Layout verified against iOS 27.0 sim cache (dyld_sim_shared_cache_arm64):
//   isa mask 0x7ffffffffffff8; cache at class+0x10; HIGH_16_BIG_ADDRS (mask<<48 | buckets);
//   bucket = {imp^class, sel}; scan decrements; no SEL_HASH_SHIFT_XOR on this build.
#include "a64.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

namespace a64 {

namespace {

constexpr u64 kIsaMask = 0x7ffffffffffff8ull;
constexpr u64 kBucketsMask = 0xffffffffffffull;
constexpr u64 kCacheOffset = 0x10;  // offsetof(objc_class, cache)
constexpr u64 kDefaultMsgSend = 0x1c0093800ull;
constexpr u64 kDefaultUncached = 0x1c0093ae0ull;
constexpr u64 kTaggedGetClass = 0x1c0093870ull;  // past b.eq nil inside NilOrTagged

u64 g_msgsend = kDefaultMsgSend;
u64 g_uncached = kDefaultUncached;
u64 g_tagged = kTaggedGetClass;
bool g_inline = false;
bool g_svc_native = false;

std::atomic<u64> g_hits{0}, g_misses{0}, g_nils{0}, g_taggeds{0};
// Plain u64 for lock-inc from emitted x86 (inline path).
alignas(8) u64 g_inline_hits = 0;
alignas(8) u64 g_inline_misses = 0;

static bool is_tagged(u64 obj) {
    return (int64_t)obj < 0;
}

static u64 cache_lookup(u64 cls, u64 sel) {
    u64 mask_and_buckets = GuestThread::rd<u64>(cls + kCacheOffset);
    u64 buckets = mask_and_buckets & kBucketsMask;
    u32 mask = (u32)(mask_and_buckets >> 48);
    if (!buckets) return 0;

    u32 idx = (u32)sel & mask;
    u64 cur = buckets + ((u64)idx << 4);
    u64 first = cur;

    for (;;) {
        u64 stored_imp = GuestThread::rd<u64>(cur);
        u64 bsel = GuestThread::rd<u64>(cur + 8);
        cur -= 16;
        if (bsel == sel) return stored_imp ^ cls;
        if (bsel == 0) return 0;
        if (cur >= buckets) continue;
        break;
    }
    cur = buckets + ((u64)mask << 4);
    for (;;) {
        u64 stored_imp = GuestThread::rd<u64>(cur);
        u64 bsel = GuestThread::rd<u64>(cur + 8);
        cur -= 16;
        if (bsel == sel) return stored_imp ^ cls;
        if (bsel == 0) return 0;
        if (cur > first) continue;
        return 0;
    }
}

void h_objc_msgSend(GuestThread &t, u32) {
    u64 obj = t.x(0);
    u64 sel = t.x(1);

    if (obj == 0) {
        g_nils.fetch_add(1, std::memory_order_relaxed);
        t.set_x(0, 0);
        t.set_x(1, 0);
        t.set_d(0, 0);
        t.set_d(1, 0);
        t.set_d(2, 0);
        t.set_d(3, 0);
        return;
    }

    if (is_tagged(obj)) {
        g_taggeds.fetch_add(1, std::memory_order_relaxed);
        t.jit->SetPC(g_tagged);
        return;
    }

    u64 raw_isa = GuestThread::rd<u64>(obj);
    u64 cls = raw_isa & kIsaMask;
    u64 imp = cache_lookup(cls, sel);
    if (imp) {
        g_hits.fetch_add(1, std::memory_order_relaxed);
        t.jit->SetPC(imp);
        return;
    }

    g_misses.fetch_add(1, std::memory_order_relaxed);
    t.set_x(15, cls);
    t.set_x(16, cls);
    t.jit->SetPC(g_uncached);
}

}  // namespace

void native_objc_prepare(const std::string &cache_path) {
    // X86CODE_A64_GUEST_OBJC=1: leave guest A64 msgSend alone (baseline).
    const char *guest = getenv("X86CODE_A64_GUEST_OBJC");
    if (guest && *guest == '1') return;

    u64 text_lo = 0, text_hi = 0;
    std::vector<u64> addrs;
    auto names = cache_exports(cache_path, "/usr/lib/objc/libobjcMsgSend.dylib", &addrs, &text_lo, &text_hi);
    for (size_t i = 0; i < names.size(); i++) {
        if (addrs[i] == ~0ull) continue;
        if (names[i] == "_objc_msgSend") g_msgsend = addrs[i];
        else if (names[i] == "__objc_msgSend_uncached") g_uncached = addrs[i];
    }
    if (!g_msgsend || !g_uncached) {
        logf("native objc: msgSend exports missing (msgsend %#lx uncached %#lx)",
             (unsigned long)g_msgsend, (unsigned long)g_uncached);
        return;
    }
    g_tagged = g_msgsend + (kTaggedGetClass - kDefaultMsgSend);

    // Default: inline x86 probe. Opt out with INLINE_OBJC=0; legacy svc with NATIVE_OBJC=1.
    const char *inline_env = getenv("X86CODE_A64_INLINE_OBJC");
    const char *native_env = getenv("X86CODE_A64_NATIVE_OBJC");
    if (native_env && *native_env == '1') {
        g_svc_native = true;
        g_inline = false;
    } else if (!inline_env || *inline_env != '0') {
        g_inline = true;
        g_svc_native = false;
    }
}

void native_objc_install() {
    if (g_svc_native) {
        static bool done;
        if (done) return;
        done = true;
        u64 addr = g_msgsend;
        u64 page = addr & ~0xfffull;
        u64 len = ((addr + 8 + 0xfff) & ~0xfffull) - page;
        if (mprotect((void *)page, len, PROT_READ | PROT_WRITE | PROT_EXEC)) {
            logf("native objc: mprotect %#lx failed", (unsigned long)page);
            g_svc_native = false;
            return;
        }
        emit_stub((uint32_t *)addr, add_thunk("_objc_msgSend", h_objc_msgSend));
        mprotect((void *)page, len, PROT_READ | PROT_EXEC);
        if (getenv("X86CODE_A64_STATS"))
            logf("native objc: patched _objc_msgSend at %#lx (svc path; uncached %#lx)",
                 (unsigned long)addr, (unsigned long)g_uncached);
        return;
    }
    // Inline path: addresses are wired into each Jit's UserConfig in GuestThread().
    if (g_inline && getenv("X86CODE_A64_STATS"))
        logf("native objc: inline x86 probe at %#lx (uncached %#lx tagged %#lx)",
             (unsigned long)g_msgsend, (unsigned long)g_uncached, (unsigned long)g_tagged);
}

void native_objc_configure_jit(Dynarmic::A64::UserConfig &c) {
    if (!g_inline) return;
    c.inline_objc_msgsend = g_msgsend;
    c.inline_objc_uncached = g_uncached;
    c.inline_objc_tagged = g_tagged;
    c.inline_objc_hit_counter = &g_inline_hits;
    c.inline_objc_miss_counter = &g_inline_misses;
}

void native_objc_stats() {
    if (g_svc_native) {
        logf("native objc (svc): hits %llu misses %llu nils %llu tagged %llu",
             (unsigned long long)g_hits.load(), (unsigned long long)g_misses.load(),
             (unsigned long long)g_nils.load(), (unsigned long long)g_taggeds.load());
    } else if (g_inline) {
        logf("native objc (inline): hits %llu misses %llu msgsend %#lx",
             (unsigned long long)g_inline_hits, (unsigned long long)g_inline_misses,
             (unsigned long)g_msgsend);
    }
}

}  // namespace a64
