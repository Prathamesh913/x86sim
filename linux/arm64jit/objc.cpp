// libarm64jit: native objc_msgSend cache lookup.
//
// Guest arm64 objc_msgSend (libobjcMsgSend.dylib in the dyld shared cache) is a tight
// cache probe: every ObjC call pays JIT translate+execute for ~15 instructions. Replace
// the entry with an svc stub; on a cache hit SetPC to the IMP (tail-call). Miss / tagged /
// nil fall back to the remaining guest code (entry bytes overwritten; slow paths live past
// the stub).
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
bool g_enabled = false;

std::atomic<u64> g_hits{0}, g_misses{0}, g_nils{0}, g_taggeds{0};

static bool is_tagged(u64 obj) {
    // MSB set ⇒ tagged (sim: looks negative); nil is 0 and already handled.
    return (int64_t)obj < 0;
}

// Probe the method cache. Returns real IMP, or 0 on miss.
// Mirrors CacheLookup in iOS 27 sim libobjcMsgSend (post-index ldp #-16, then wrap).
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
        cur -= 16;  // post-index
        if (bsel == sel) return stored_imp ^ cls;  // cached as imp^class
        if (bsel == 0) return 0;
        if (cur >= buckets) continue;
        break;
    }
    // Wrap to last bucket; stop once we pass the first probed slot.
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
        // Leave PC at stub+4 (ret) → return to caller.
        return;
    }

    if (is_tagged(obj)) {
        g_taggeds.fetch_add(1, std::memory_order_relaxed);
        // Guest NilOrTagged body past the nil b.eq: loads tagged class, then cache lookup.
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
    // Match CacheLookup miss ABI: x15 = class, x16 = class (uncached may set bit 2).
    t.set_x(15, cls);
    t.set_x(16, cls);
    t.jit->SetPC(g_uncached);
}

}  // namespace

void native_objc_prepare(const std::string &cache_path) {
    // Opt-in: the svc boundary around a 15-instruction cache probe is slower than
    // running those instructions under the JIT (objcswift msgSend ~537 ms native
    // vs ~291 ms guest). Keep the path for experiments / a future inlined emit.
    // X86CODE_A64_NATIVE_OBJC=1 enables; X86CODE_A64_GUEST_OBJC=1 forces guest.
    const char *guest = getenv("X86CODE_A64_GUEST_OBJC");
    if (guest && *guest == '1') return;
    const char *on = getenv("X86CODE_A64_NATIVE_OBJC");
    if (!on || *on != '1') return;

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
    // Tagged path offset is fixed relative to msgSend in this cache build (NilOrTagged).
    g_tagged = g_msgsend + (kTaggedGetClass - kDefaultMsgSend);
    g_enabled = true;
}

void native_objc_install() {
    if (!g_enabled) return;
    static bool done;
    if (done) return;
    done = true;

    u64 addr = g_msgsend;
    u64 page = addr & ~0xfffull;
    u64 len = ((addr + 8 + 0xfff) & ~0xfffull) - page;
    if (mprotect((void *)page, len, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        logf("native objc: mprotect %#lx failed", (unsigned long)page);
        g_enabled = false;
        return;
    }
    emit_stub((uint32_t *)addr, add_thunk("_objc_msgSend", h_objc_msgSend));
    mprotect((void *)page, len, PROT_READ | PROT_EXEC);
    if (getenv("X86CODE_A64_STATS"))
        logf("native objc: patched _objc_msgSend at %#lx (uncached %#lx)",
             (unsigned long)addr, (unsigned long)g_uncached);
}

void native_objc_stats() {
    if (!g_enabled) return;
    logf("native objc: hits %llu misses %llu nils %llu tagged %llu",
         (unsigned long long)g_hits.load(), (unsigned long long)g_misses.load(),
         (unsigned long long)g_nils.load(), (unsigned long long)g_taggeds.load());
}

}  // namespace a64
