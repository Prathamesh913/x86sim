// libarm64jit: guest threads. Each native thread that runs guest code gets its own dynarmic Jit (guest registers) and an 8 MB guest stack; the translated code is shared by all threads of the process (shared_code below).
// Native code may call back into guest code on the same thread (qsort comparators, pthread_once, TSD destructors...):
// call() then runs a nested Jit::Run (re-entrant in our dynarmic fork) on a stack frame 128 bytes below the guest's
// current SP and restores the full guest state afterwards.
#include <dlfcn.h>
#include <new>
#include <sys/personality.h>
#include "a64.h"
#include <atomic>
#include <algorithm>
#include <mutex>
#include <map>
#include <thread>
#include <vector>
#include <cstring>
#include <csignal>
#include <pthread.h>
#include <sys/uio.h>
#include <malloc.h>
#include <sched.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/mman.h>
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace a64 {
extern bool g_step;

static Dynarmic::ExclusiveMonitor *monitor() {
    static auto *m = new Dynarmic::ExclusiveMonitor(64);
    return m;
}
static std::atomic<size_t> g_next_cpu{0};
static std::atomic<int> g_live_threads{0};  // GuestThreads alive (profiling)
static thread_local GuestThread *t_self;
static thread_local struct Owner { GuestThread *t = nullptr; ~Owner() { delete t; } } t_owner;

// Darling's threads end without running the ELF's thread_local destructors: a native (Darling) TSD key with a
// destructor frees the thread's GuestThread (Jit, code cache, guest stack) when the thread exits. Before this every
// exited workqueue thread leaked ~20 MB.
// Guest pthread key destructors (guest function addresses; 0 = none), see h_key_create.
static std::atomic<u64> g_key_dtor[GuestThread::kGuestTsdSlots];
void set_guest_key_destructor(u64 key, u64 guest_dtor) {
    if (key < GuestThread::kGuestTsdSlots) g_key_dtor[key].store(guest_dtor);
}
// As libpthread's _pthread_tsd_cleanup: up to PTHREAD_DESTRUCTOR_ITERATIONS (4) passes over the keys, each value
// cleared before its destructor runs (a destructor may set values again). Slots 0-9 are the reserved shared ones.
void GuestThread::run_guest_tsd_destructors() {
    for (int pass = 0; pass < 4; pass++) {
        bool any = false;
        for (int k = 10; k < kGuestTsdSlots; k++) {
            const u64 v = tsd[k], d = g_key_dtor[k].load(std::memory_order_relaxed);
            if (!v || !d) continue;
            tsd[k] = 0;
            any = true;
            call(d, {v});
        }
        if (!any) break;
    }
}

static void destroy_guest_thread(void *p) {
    auto *t = (GuestThread *)p;
    if (t->depth == 0 && t_self == t) {
        t->run_guest_tsd_destructors();
        t_self = nullptr; t_owner.t = nullptr; delete t;
    }
}
// X86CODE_A64_GSAMPLE=<exe name> (debug): a host thread samples the process's main guest thread (from dyld_sim start) every 5 ms (last
// block PC, LR and 6 frame-pointer frames, read with process_vm_readv) and logs the 15 most frequent stacks every 10 s.
// For "busy but nothing visible" loops that perf cannot attribute (JIT frames have no host unwind info).
// X86CODE_A64_GSAMPLE_ALL=1: every guest thread of the process is sampled, and only samples running guest code (not
// in a thunk) count: the busy worker thread instead of an idle main thread. =2: blocked threads too (lock holders).
static std::mutex g_gs_mu;
static std::vector<GuestThread *> g_gs_threads;
static int g_gs_all;  // 1: threads in guest code; 2: every thread (also blocked in a thunk: lock holders)
static void gsample_register(GuestThread *t) {
    if (!g_gs_all) return;
    std::lock_guard l(g_gs_mu); g_gs_threads.push_back(t);
}
void gsample_forget(GuestThread *t) {
    if (!g_gs_all) return;
    std::lock_guard l(g_gs_mu);
    g_gs_threads.erase(std::remove(g_gs_threads.begin(), g_gs_threads.end(), t), g_gs_threads.end());
}
void gsample_start(GuestThread *t) {
    const char *want = getenv("X86CODE_A64_GSAMPLE"), *exe = getenv("X86CODE_A64_EXE");
    if (!want || !exe) return;
    const char *b = strrchr(exe, '/');
    if (strcmp(want, b ? b + 1 : exe) != 0) return;
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    g_gs_all = getenv("X86CODE_A64_GSAMPLE_ALL") ? atoi(getenv("X86CODE_A64_GSAMPLE_ALL")) : 0;
    if (g_gs_all) gsample_register(t);
    std::thread([t]() mutable {
        g_non_darling_thread = true;
        auto rd = [](u64 a) -> u64 {
            u64 v = 0;
            struct iovec lo{&v, 8}, re{reinterpret_cast<void *>(a), 8};
            return syscall(SYS_process_vm_readv, getpid(), &lo, 1, &re, 1, 0) == 8 ? v : 0;
        };
        std::map<std::vector<u64>, u32> counts;
        std::map<u64, u32> incl;
        u32 n = 0;
        const u32 period = getenv("X86CODE_A64_GSAMPLE_PERIOD") ? atoi(getenv("X86CODE_A64_GSAMPLE_PERIOD")) : 2000;
        const char *sk = getenv("X86CODE_A64_GSAMPLE_SKIP");  // a PC to leave out (the run loop's mach_msg wait)
        const u64 skip = sk ? strtoull(sk, nullptr, 16) : 0;
        const bool trace = getenv("X86CODE_A64_GSAMPLE_TRACE") != nullptr;
        const int depth = getenv("X86CODE_A64_GSAMPLE_DEPTH") ? atoi(getenv("X86CODE_A64_GSAMPLE_DEPTH")) : 6;
        // logf skips the per-process log on this (non-Darling) thread: X86CODE_A64_GSAMPLE_OUT=<Linux path> writes
        // <path>.<pid> with glibc instead (a job's stderr is often gone)
        FILE *out = nullptr;
        if (const char *o = getenv("X86CODE_A64_GSAMPLE_OUT"))
            out = fopen((std::string(o) + "." + std::to_string(getpid())).c_str(), "a");
        timespec ts0; clock_gettime(CLOCK_MONOTONIC, &ts0);
        auto emit = [out, ts0](const char *line) {
            if (!out) { logf("%s", line); return; }
            timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            fprintf(out, "%.2f %s\n", (ts.tv_sec - ts0.tv_sec) + (ts.tv_nsec - ts0.tv_nsec) / 1e9, line);
            fflush(out);
        };
        for (;;) {
            usleep(5000);
            std::unique_lock<std::mutex> lk(g_gs_mu, std::defer_lock);
            if (g_gs_all) {  // one sample: a thread in guest code, round robin from a random start
                lk.lock();
                GuestThread *pick = nullptr;
                const size_t m = g_gs_threads.size();
                for (size_t k = 0, r = m ? (size_t)random() % m : 0; k < m && !pick; k++) {
                    GuestThread *c = g_gs_threads[(r + k) % m];
                    if (c->jit && (c->in_thunk < 0 || g_gs_all == 2)) pick = c;
                }
                if (!pick) continue;
                t = pick;
            }
            if (!t->jit) continue;
            // a native call in progress is recorded as its thunk index (high marker) in front of the guest pc
            const int32_t th = t->in_thunk;
            std::vector<u64> st{th >= 0 ? 0xF000000000000000ull | (u32)th : 0, t->jit->GetPC(), t->x(30)};
            if (skip && st[0] == skip) continue;
            u64 fp = t->x(29);
            for (int k = 0; k < depth && fp > 0x10000 && !(fp & 7); k++) {
                st.push_back(rd(fp + 8));
                const u64 next = rd(fp);
                if (next <= fp) break;
                fp = next;
            }
            if (trace) {  // X86CODE_A64_GSAMPLE_TRACE=1: every sample with its CLOCK_REALTIME ns (lines up with simhid/ring)
                timespec tr; clock_gettime(CLOCK_REALTIME, &tr);
                char line[1024];
                int o = snprintf(line, sizeof line, "T %llu", (unsigned long long)tr.tv_sec * 1000000000ull + tr.tv_nsec);
                for (u64 a : st) {
                    if (a >> 60 == 0xF) o += snprintf(line + o, sizeof line - o > 0 ? sizeof line - o : 0, " [%s]", g_thunks[a & 0xffffffff].name.c_str());
                    else o += snprintf(line + o, sizeof line - o > 0 ? sizeof line - o : 0, " %#lx", (unsigned long)a);
                }
                if (out) { fputs(line, out); fputc('\n', out); }
            }
            counts[st]++;
            for (size_t i = 0; i < st.size(); i++)
                if (std::find(st.begin(), st.begin() + i, st[i]) == st.begin() + i) incl[st[i]]++;
            if (++n % period) continue;
            std::vector<std::pair<u32, const std::vector<u64> *>> top;
            for (auto &[k, v] : counts) top.push_back({v, &k});
            std::sort(top.begin(), top.end(), [](auto &a, auto &b) { return a.first > b.first; });
            { char l[64]; snprintf(l, sizeof l, "gsample: %u samples", n); emit(l); }
            for (size_t i = 0; i < top.size() && i < 15; i++) {
                char line[1024];
                int o = snprintf(line, sizeof line, "gsample %4u:", top[i].first);
                for (u64 a : *top[i].second) {
                    if (a >> 60 == 0xF) o += snprintf(line + o, sizeof line - o > 0 ? sizeof line - o : 0, " [%s]", g_thunks[a & 0xffffffff].name.c_str());
                    else if (a) o += snprintf(line + o, sizeof line - o > 0 ? sizeof line - o : 0, " %#lx", (unsigned long)a);
                }
                emit(line);
            }
            counts.clear();
            // inclusive: every frame address with the samples it appears in (attribute to images offline)
            std::vector<std::pair<u32, u64>> inc;
            for (auto &[a, v] : incl) inc.push_back({v, a});
            std::sort(inc.rbegin(), inc.rend());
            for (size_t i = 0; i < inc.size() && i < 80; i++) {
                char l[64]; snprintf(l, sizeof l, "gsample incl %u %#lx", inc[i].first, (unsigned long)inc[i].second); emit(l);
            }
            incl.clear();
        }
    }).detach();
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
}

GuestThread &GuestThread::self() {
    if (!t_self) {
        t_owner.t = new GuestThread(); t_self = t_owner.t;
        gsample_register(t_self);
        static unsigned long key = [] {
            unsigned long k = 0;
            auto *create = (int (*)(unsigned long *, void (*)(void *)))dsym("pthread_key_create");
            if (create) create(&k, destroy_guest_thread);
            return k;
        }();
        static auto *setspec = (int (*)(unsigned long, const void *))dsym("pthread_setspecific");
        if (key && setspec) setspec(key, t_self);
    }
    return *t_self;
}

GuestThread *GuestThread::current() { return t_self; }

// Stage 3: one translation cache per process, shared by every guest thread (dynarmic's shared_translations: the
// code reads tpidr/tpidrro and the exclusive-monitor slot from the running thread's JitState). The code calls these
// process-wide callbacks, which forward to the calling thread's GuestThread. X86CODE_A64_SHARED=0: one cache per
// thread (stage 2). Shared code is never freed while the process runs (threads blocked in native calls return
// into it), so the cache is a large reservation: only the code actually emitted is resident.
namespace {
// Speed 9: libswift_Concurrency's dispatch executor (the swift_task_enqueueGlobalWithDeadline path) converts a
// deadline from ns to mach ticks with Apple silicon's timebase inlined (ns * 3 / 125). Our guest timebase is 1/1 (mach
// time counts ns, like Darling's), so every Swift sleep (Task.sleep, Clock.sleep, ...) fired at once: Settings'
// search-field task spun its main thread (~10k wakeups/s, ~90% of a core while idle) and its placeholder crossfade
// never settled. The two words that pick the conversion branch read as "mov x21, x8; b <join>" (deadline unchanged).
// Only when the original words are there (iOS 27.0 24A434 cache); guest memory is not written (MemoryReadCode, like
// swiftconf), so the AOT builder sees the same code. X86CODE_A64_SWIFT_TIMEBASE=0 turns it off (rebuild the image).
constexpr u64 kSwiftDeadlineConv = 0x1c156d4ac;
std::optional<uint32_t> swift_timebase_read_code(u64 a) {
    if (a != kSwiftDeadlineConv && a != kSwiftDeadlineConv + 4) return std::nullopt;
    static const bool ok = [] {
        const char *e = getenv("X86CODE_A64_SWIFT_TIMEBASE");
        if (e && atoi(e) == 0) return false;
        if (timebase_apple()) return false;  // the guest's timebase is 125/3: the inlined conversion is right
        // cmp x8, x9 ; b.lo (x*3 first) ; ... and the join branch both conversions take (b -> 0x1c156d3bc)
        const bool match = GuestThread::rd<uint32_t>(kSwiftDeadlineConv) == 0xeb09011fu &&
                           GuestThread::rd<uint32_t>(kSwiftDeadlineConv + 4) == 0x54000663u &&
                           GuestThread::rd<uint32_t>(0x1c156d4d8) == 0x17ffffb9u;
        if (!match) logf("swift timebase: libswift_Concurrency deadline conversion not found; Task.sleep stays early");
        return match;
    }();
    if (!ok) return std::nullopt;
    return a == kSwiftDeadlineConv ? 0xaa0803f5u /* mov x21, x8 */ : 0x17ffffc3u /* b 0x1c156d3bc */;
}

struct Forward final : Dynarmic::A64::UserCallbacks {
    static GuestThread &T() { return *t_self; }
    std::optional<uint32_t> MemoryReadCode(u64 a) override {
        if (auto w = swiftconf_read_code(a)) return w;
        if (auto w = swift_timebase_read_code(a)) return w;
        return GuestThread::rd<uint32_t>(a);
    }
    uint8_t MemoryRead8(u64 a) override { return GuestThread::rd<uint8_t>(a); }
    uint16_t MemoryRead16(u64 a) override { return GuestThread::rd<uint16_t>(a); }
    uint32_t MemoryRead32(u64 a) override { return GuestThread::rd<uint32_t>(a); }
    u64 MemoryRead64(u64 a) override { return GuestThread::rd<u64>(a); }
    // Speed 17: an import stub's GOT slot, for the guarded direct link (A64StubLinkPass). Only slots and targets in
    // the persisted window (shared cache + placed images: fixed per cache key), read without faulting. Unbound
    // slots (chained-fixup encodings in the AOT builder's unbound placed images) fall outside the window.
    std::optional<u64> MemoryReadConstant64(u64 a) override {
        constexpr u64 lo = 0x1c0000000ull, hi = 0x2c0000000ull;
        if (a < lo || a + 8 > hi || (a & 7)) return std::nullopt;
        u64 v = 0;
        iovec l{&v, 8}, r{(void *)a, 8};
        if (process_vm_readv(getpid(), &l, 1, &r, 1, 0) != 8) return std::nullopt;
        if (v < lo || v >= hi) return std::nullopt;
        return v;
    }
    Dynarmic::A64::Vector MemoryRead128(u64 a) override { return GuestThread::rd<Dynarmic::A64::Vector>(a); }
    void MemoryWrite8(u64 a, uint8_t v) override { GuestThread::wr(a, v); }
    void MemoryWrite16(u64 a, uint16_t v) override { GuestThread::wr(a, v); }
    void MemoryWrite32(u64 a, uint32_t v) override { GuestThread::wr(a, v); }
    void MemoryWrite64(u64 a, u64 v) override { GuestThread::wr(a, v); }
    void MemoryWrite128(u64 a, Dynarmic::A64::Vector v) override { GuestThread::wr(a, v); }
    bool MemoryWriteExclusive8(u64 a, uint8_t v, uint8_t e) override { return T().MemoryWriteExclusive8(a, v, e); }
    bool MemoryWriteExclusive16(u64 a, uint16_t v, uint16_t e) override { return T().MemoryWriteExclusive16(a, v, e); }
    bool MemoryWriteExclusive32(u64 a, uint32_t v, uint32_t e) override { return T().MemoryWriteExclusive32(a, v, e); }
    bool MemoryWriteExclusive64(u64 a, u64 v, u64 e) override { return T().MemoryWriteExclusive64(a, v, e); }
    bool MemoryWriteExclusive128(u64 a, Dynarmic::A64::Vector v, Dynarmic::A64::Vector e) override { return T().MemoryWriteExclusive128(a, v, e); }
    void InterpreterFallback(u64 pc, size_t n) override { T().InterpreterFallback(pc, n); }
    void CallSVC(uint32_t swi) override { T().CallSVC(swi); }
    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception e) override { T().ExceptionRaised(pc, e); }
    void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, u64 value) override {
        T().InstructionCacheOperationRaised(op, value);
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return ~0ull; }
    u64 GetCNTPCT() override { return T().GetCNTPCT(); }
};
}

static bool shared_translations() {
    static const bool on = !(getenv("X86CODE_A64_SHARED") && atoi(getenv("X86CODE_A64_SHARED")) == 0);
    return on;
}

// Speed 6 (shared AOT image): translated code embeds the code cache address, the FastDispatch table (mapped after the
// code cache), the callbacks object and the inline objc counters. All of them sit at fixed addresses so the relocated
// image is byte-identical in every process: the code cache at kNoAslrCodeBase when ASLR is off (launchd_sim's
// children; the library's own address still differs by executable), else 1 GB below this library (rel32 reach to it
// < 2 GB), and the rest in a fixed data page 2 MB below the code cache.
constexpr u64 kNoAslrCodeBase = 0x7fffa0000000ull;
// Speed 18: the shared code cache holds the AOT image plus room for this process's own translations. Default 768 MB;
// with a larger image, image + 384 MB, up to 1344 MB: the cache must stay within rel32 reach of libarm64jit and glibc (near
// host calls in the prelude; the image's prelude layout depends on it). X86CODE_A64_CODECACHE_MB overrides.
static u64 code_cache_mb() {
    static const u64 mb = []() -> u64 {
        if (const char *e = getenv("X86CODE_A64_CODECACHE_MB")) { const int v = atoi(e); if (v >= 64) return std::min(v, 1344); }
        const u64 img = jitcache_image_code_bytes() >> 20;
        return std::clamp<u64>(img + 384, 768, 1344);
    }();
    return mb;
}
static u64 code_cache_base() {
    static const u64 base = []() -> u64 {
        // the cache ends where the 768 MB one always ended (kNoAslrCodeBase + 768 MB) and grows downwards
        if (personality(0xffffffff) & ADDR_NO_RANDOMIZE) return kNoAslrCodeBase + (768ull << 20) - (code_cache_mb() << 20);
        Dl_info di{};
        // below libarm64jit, as close as fits (Speed 18 builders use slabs of up to 1344 MB)
        return dladdr((void *)&code_cache_base, &di) && di.dli_fbase
                   ? ((u64)di.dli_fbase - std::max<u64>(1ull << 30, (code_cache_mb() << 20) + (64ull << 20))) & ~0xfffffull : 0;
    }();
    return base;
}
FixedData &fixed_data() {
    static FixedData *fd = [] {
        void *p = MAP_FAILED;
        if (const u64 b = code_cache_base())
            p = mmap((void *)(b - 0x200000), sizeof(FixedData), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) p = mmap(nullptr, sizeof(FixedData), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return new (p) FixedData();
    }();
    return *fd;
}

static std::shared_ptr<Dynarmic::A64::Jit::SharedCode> shared_code(const Dynarmic::A64::UserConfig &thread_cfg) {
    static std::shared_ptr<Dynarmic::A64::Jit::SharedCode> code = [&] {
        static_assert(sizeof(Forward) <= sizeof(FixedData::callbacks));
        Dynarmic::A64::UserConfig c = thread_cfg;
        c.callbacks = new (fixed_data().callbacks) Forward;
        c.shared_translations = true;
        c.tpidr_el0 = nullptr; c.tpidrro_el0 = nullptr;
        // Shared BlockLinking uses atomic 8-byte patch heads (see EmitSharedPatchSite). Required for
        // boot-to-home: with linking off, SpringBoard livelocks in the dispatcher (billions of
        // lookups, idle black). X86CODE_A64_LINK=0 disables linking for experiments.
        // Fastmem fault "recompile" Invalidates/Unpatches from the SIGSEGV handler — even with
        // shared_mutex held that still patches live code; redirect the fault via FakeCall only
        // (recompile_on_*_failure = false). FastDispatch stays (16-byte atomic data). RSB is off by
        // default: Invalidate clears the shared map/FD but cannot reset every thread's RSB; set
        // X86CODE_A64_RSB=1 to re-enable for experiments.
        if (getenv("X86CODE_A64_LINK") && atoi(getenv("X86CODE_A64_LINK")) == 0) {
            c.optimizations &= ~Dynarmic::OptimizationFlag::BlockLinking;
        }
        if (!(getenv("X86CODE_A64_RSB") && atoi(getenv("X86CODE_A64_RSB")) == 1)) {
            c.optimizations &= ~Dynarmic::OptimizationFlag::ReturnStackBuffer;
        }
        // X86CODE_A64_SAFE=1: also drop FastDispatch (narrows shared races further for experiments).
        if (getenv("X86CODE_A64_SAFE") && atoi(getenv("X86CODE_A64_SAFE")) == 1) {
            c.optimizations &= ~Dynarmic::OptimizationFlag::FastDispatch;
            c.optimizations &= ~Dynarmic::OptimizationFlag::ReturnStackBuffer;
        }
        // X86CODE_A64_FASTDISPATCH=0: drop FastDispatch only (keep other SAFE-independent opts).
        if (getenv("X86CODE_A64_FASTDISPATCH") && atoi(getenv("X86CODE_A64_FASTDISPATCH")) == 0) {
            c.optimizations &= ~Dynarmic::OptimizationFlag::FastDispatch;
        }
        // Speed 20: LDXR/STXR without the process-wide monitor: STXR is a lock cmpxchg against the value its LDXR
        // read, with a reservation private to the thread (QEMU user-mode semantics; only ABA differs from real LL/SC).
        // The global monitor took a spinlock and scanned all 64 slots on every STXR: objc retain/release were ~1/3 of
        // SpringBoard's CPU on a tap. X86CODE_A64_STRICT_MONITOR=1 restores it (a code-shaping flag: key bit 19).
        if (!strict_monitor()) {
            c.unsafe_optimizations = true;
            c.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        }
        c.recompile_on_fastmem_failure = false;
        c.recompile_on_exclusive_fastmem_failure = false;
        // Per process (reserved; only emitted pages are resident). Stage 2's per-thread 8 MB is too small here.
        // Speed 8: 384 MB (was 256): the AOT image with MTLCompilerService and iconservicesagent holds ~230 MB of
        // code, and SpringBoard's own new code no longer fit ("shared code cache full").
        // Speed 12: 768 MB. The image trained on SpringBoard after home, Settings sub-pages, the keyboard and the stock
        // apps holds ~380 MB of code and filled 384 MB (SpringBoard aborted in a loop).
        c.code_cache_size = (size_t)code_cache_mb() << 20;
        if (const u64 b = code_cache_base()) Dynarmic::A64::Jit::SetCodeCacheAddress(b);
        auto sc = Dynarmic::A64::Jit::MakeSharedCode(c);
        jitcache_set_shared(sc);
        jitcache_try_load(*sc);
        return sc;
    }();
    return code;
}

static std::unique_ptr<Dynarmic::A64::Jit> make_jit(const Dynarmic::A64::UserConfig &cfg) {
    if (shared_translations()) return std::make_unique<Dynarmic::A64::Jit>(cfg, shared_code(cfg));
    return std::make_unique<Dynarmic::A64::Jit>(cfg);
}

GuestThread::GuestThread() {
    // The guest has its own TSD (tpidrro_el0): slots 0-9 (pthread_self, errno, mach thread self, ...) are copied from
    // Darling's native TSD (the gs base on x86_64); keys 10+ are the guest's alone. Sharing the native array let
    // Darling's own libobjc/libdispatch destructors free guest values at thread exit (SIGABRT in every daemon).
    // Guest key destructors run at thread exit (run_guest_tsd_destructors).
    unsigned long gs = 0;
    syscall(SYS_arch_prctl, ARCH_GET_GS, &gs);
    tsd = (u64 *)calloc(kGuestTsdSlots, 8);
    native_tsd = (u64 *)gs;
    if (gs) memcpy(tsd, (void *)gs, 10 * 8);
    tpidrro = (u64)tsd;
    Dynarmic::A64::UserConfig &c = cfg;
    c.callbacks = this;
    c.processor_id = g_next_cpu++ % 64;
    c.global_monitor = monitor();
    c.fastmem_pointer = 0;               // identity mapping: guest address == host address
    c.fastmem_address_space_bits = 47;   // x86-64 user space
    c.silently_mirror_fastmem = false;
    c.fastmem_exclusive_access = true;
    c.enable_cycle_counting = false;
    c.wall_clock_cntpct = true;
    c.cntfrq_el0 = timebase_apple() ? 24000000 : 1000000000;  // ns like Darling's mach time (1/1), or 24 MHz (125/3)
    c.tpidr_el0 = &tpidr;
    c.tpidrro_el0 = &tpidrro;
    // Hybrid svc;ret (frontend): a thunk block ends in FastDispatchHint instead of PopRSBHint. With RSB off
    // (shared translations) PopRSBHint is ReturnFromRunCode, so every libsystem call left Run(). Opt-in for now.
    c.fuse_svc_ret = getenv("X86CODE_A64_FUSE_SVC") && atoi(getenv("X86CODE_A64_FUSE_SVC")) == 1;
    // Speed 17: guarded direct links for import stubs (X86CODE_A64_STUBLINK=0: off)
    c.link_import_stubs = !(getenv("X86CODE_A64_STUBLINK") && atoi(getenv("X86CODE_A64_STUBLINK")) == 0);
    native_objc_configure_jit(c);
    // Per-thread caches (X86CODE_A64_SHARED=0): a full cache is cleared, which also drops its block metadata (in
    // glibc malloc, ~3x the code). 16 MB bounds a thread at ~60 MB.
    c.code_cache_size = (getenv("X86CODE_A64_CODECACHE_MB") ? atoi(getenv("X86CODE_A64_CODECACHE_MB")) : 16) << 20;
    static const bool rss_debug = getenv("X86CODE_A64_RSS") != nullptr;
    auto rss = [] { long v = 0; if (FILE *f = fopen("/proc/self/statm", "r")) { long a, b, c2; if (fscanf(f, "%ld %ld %ld", &a, &b, &c2) == 3) v = (b - c2) * 4; fclose(f); } return v; };
    long before = rss_debug ? rss() : 0;
    jit = make_jit(c);
    if (rss_debug) { struct mallinfo2 mi = mallinfo2(); logf("new Jit: anon RSS +%ld kB; glibc mmapped %zu MB in %zu blocks, arena %zu MB", rss() - before, mi.hblkhd >> 20, mi.hblks, mi.arena >> 20); }
    g_live_threads++;
    stack_size = 8u << 20;
    stack_base = (u64)mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (stack_base == (u64)MAP_FAILED) die("cannot allocate a guest stack");
}
// After a longjmp out of call() (pthread_exit) the Jit still counts as executing: its state is abandoned (leaked,
// destroying it could assert) and the thread gets a fresh one for whatever guest code still runs on it (TSD
// destructors at thread exit).
void GuestThread::reset_after_unwind() {
    depth = 0; done = nullptr; in_thunk = -1;
    (void)jit.release();
    jit = make_jit(cfg);
}
GuestThread::~GuestThread() { gsample_forget(this); g_live_threads--; munmap((void *)stack_base, stack_size); free(tsd); }

NRet GuestThread::call(u64 pc, const u64 *ints, int nints, const u64 *fps, int nfps) {
    struct Saved {
        std::array<u64, 31> r; u64 sp, pc; std::array<Dynarmic::A64::Vector, 32> v; u32 pstate, fpcr, fpsr;
    } saved;
    const bool nested = depth > 0;
    if (nested) {
        saved.r = jit->GetRegisters(); saved.sp = jit->GetSP(); saved.pc = jit->GetPC(); saved.v = jit->GetVectors();
        saved.pstate = jit->GetPstate(); saved.fpcr = jit->GetFpcr(); saved.fpsr = jit->GetFpsr();
    }
    for (int i = 0; i < nints && i < 8; i++) jit->SetRegister(i, ints[i]);
    for (int i = 0; i < nfps; i++) jit->SetVector(i, {fps[i], 0});
    jit->SetRegister(30, g_exit_stub);
    jit->SetSP(nested ? ((saved.sp - 128) & ~15ull) : ((stack_base + stack_size - 64) & ~15ull));
    for (int i = 8; i < nints; i++) wr<u64>(jit->GetSP() + 8 * (i - 8), ints[i]);  // 9th+ arguments: stack slots
    jit->SetPC(pc);
    bool finished = false;
    bool *outer_done = done;
    done = &finished;
    depth++;
    // X86CODE_A64_EXEC_LOCK=1: one process-wide mutex around every Run (bisect: cache/linking race vs
    // per-thread state leak). Shared-mode only; huge slowdown, for experiments.
    static std::mutex *exec_lock = []() -> std::mutex * {
        if (!(getenv("X86CODE_A64_EXEC_LOCK") && atoi(getenv("X86CODE_A64_EXEC_LOCK")) == 1)) return nullptr;
        return new std::mutex;
    }();
    if (g_step && depth == 1) while (!finished) jit->Step();
    else while (!finished) {
        if (exec_lock) {
            std::lock_guard<std::mutex> g(*exec_lock);
            jit->Run();
        } else {
            jit->Run();
        }
        if (sig_pending.load(std::memory_order_relaxed)) deliver_signals(*this);
    }
    depth--;
    done = outer_done;
    NRet r{jit->GetRegister(0), jit->GetRegister(1), jit->GetVector(0)[0], jit->GetVector(1)[0]};
    if (nested) {
        jit->SetRegisters(saved.r); jit->SetSP(saved.sp); jit->SetPC(saved.pc); jit->SetVectors(saved.v);
        jit->SetPstate(saved.pstate); jit->SetFpcr(saved.fpcr); jit->SetFpsr(saved.fpsr);
    }
    return r;
}

bool GuestThread::MemoryWriteExclusive128(u64 a, Dynarmic::A64::Vector v, Dynarmic::A64::Vector e) {
    unsigned __int128 ev = ((unsigned __int128)e[1] << 64) | e[0], nv = ((unsigned __int128)v[1] << 64) | v[0];
    return __atomic_compare_exchange_n((unsigned __int128 *)(a & kTbi), &ev, nv, false, 5, 5);
}

bool g_count_thunks = getenv("X86CODE_A64_STATS") != nullptr;
u64 g_thunk_counts[0x10000];

// X86CODE_A64_PROFILE=1: every 10 s a line in the process log: wall and process CPU time since start, time spent
// translating (shared cache), blocks, guest instructions and x86 bytes emitted, dispatcher lookups, live threads.
static const bool g_profile = getenv("X86CODE_A64_PROFILE") != nullptr;
static void profile_tick() {
    static const u64 start = [] { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + t.tv_nsec; }();
    static std::atomic<u64> last{0};
    timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    const u64 now = (u64)t.tv_sec * 1000000000ull + t.tv_nsec;
    u64 prev = last.load();
    if (now - prev < 10000000000ull || !last.compare_exchange_strong(prev, now)) return;
    timespec c; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c);
    Dynarmic::A64::Jit::SharedStats st{};
    if (shared_translations()) st = Dynarmic::A64::Jit::GetSharedStats(*shared_code(GuestThread::self().cfg));
    logf("profile: wall %.1fs cpu %.1fs translate %.1fs blocks %llu insns %llu code %.1f MB lookups %llu threads %d",
         (now - start) / 1e9, c.tv_sec + c.tv_nsec / 1e9, st.translate_ns / 1e9, (unsigned long long)st.blocks,
         (unsigned long long)st.guest_instructions, st.code_bytes / 1048576.0, (unsigned long long)st.lookups, g_live_threads.load());
}

// Called from host.cpp memlog_once / SIGUSR1. Safe when this tid has no GuestThread.
void memlog_shared_stats() {
    if (!shared_translations() || !GuestThread::current()) return;
    auto st = Dynarmic::A64::Jit::GetSharedStats(*shared_code(GuestThread::self().cfg));
    logf("memlog: shared blocks %llu insns %llu code %.1f MB translate %.1fs lookups %llu threads %d",
         (unsigned long long)st.blocks, (unsigned long long)st.guest_instructions,
         st.code_bytes / 1048576.0, st.translate_ns / 1e9,
         (unsigned long long)st.lookups, g_live_threads.load());
    logf("memlog: maps (MB) %s", Dynarmic::A64::Jit::GetMemoryReport(*shared_code(GuestThread::self().cfg)).c_str());
    native_objc_stats();
    jitcache_save_if_shared();
}

void GuestThread::CallSVC(uint32_t swi) {
    const u64 pc0 = jit->GetPC();  // svc+4
    if (swi == kExitSvc) { *done = true; jit->HaltExecution(); return; }
    if (swi == kSwiftConfSvc) { swiftconf_step(*this); return; }
    if (swi == kLinuxSvc) {  // our in-simulator code's escape to a Linux syscall (x8 = x86_64 number, x0-x5 args)
        set_x(0, (u64)syscall((long)x(8), x(0), x(1), x(2), x(3), x(4), x(5)));
    } else {
        svc_count++;
        if (g_profile && (svc_count & 1023) == 0) profile_tick();
        if (swi >= thunk_count()) die("svc #%u at %#lx: no such thunk", swi, (unsigned long)jit->GetPC() - 4);
        if (g_count_thunks) __atomic_fetch_add(&g_thunk_counts[swi], 1, __ATOMIC_RELAXED);
        if (g_trace) {
            const std::string &nm = g_thunks[swi].name;
            bool path = nm.find("open") != std::string::npos || nm.find("stat") != std::string::npos ||
                        nm.find("access") != std::string::npos || nm.find("getattr") != std::string::npos ||
                        nm.find("readlink") != std::string::npos || nm.find("realpath") != std::string::npos;
            if (path && (nm.find("at") != std::string::npos && nm.find("stat") == std::string::npos ? true : false) && x(1) > 0x10000)
                logf("-> %s(%#lx, \"%.200s\", %#lx)", nm.c_str(), (unsigned long)x(0), (const char *)x(1), (unsigned long)x(2));
            else if (path && nm.find("at") != std::string::npos && nm.find("stat") == std::string::npos)
                logf("-> %s(%#lx, %#lx, %#lx)", nm.c_str(), (unsigned long)x(0), (unsigned long)x(1), (unsigned long)x(2));
            else if (path && x(0) > 0x10000) logf("-> %s(\"%.200s\", %#lx, %#lx)", nm.c_str(), (const char *)x(0), (unsigned long)x(1), (unsigned long)x(2));
            else logf("-> %s(%#lx, %#lx, %#lx)", nm.c_str(), (unsigned long)x(0), (unsigned long)x(1), (unsigned long)x(2));
        }
        // X86CODE_A64_SVC_TIME=1: accumulate rdtsc around the handler body (excludes dispatcher).
        static const bool svc_time = getenv("X86CODE_A64_SVC_TIME") != nullptr;
        static std::atomic<u64> svc_cycles{0}, svc_timed{0};
        u64 t0 = 0;
        if (svc_time) t0 = __builtin_ia32_rdtsc();
        int32_t outer_thunk = in_thunk; in_thunk = (int32_t)swi;
        tsd_to_native();
        g_thunks[swi].fn(*this, swi);
        tsd_from_native();
        in_thunk = outer_thunk;
        if (svc_time) {
            u64 dt = __builtin_ia32_rdtsc() - t0;
            u64 n = svc_timed.fetch_add(1, std::memory_order_relaxed) + 1;
            u64 sum = svc_cycles.fetch_add(dt, std::memory_order_relaxed) + dt;
            if ((n & 0xfffff) == 0)
                logf("svc timing: %llu calls, avg %.1f cycles (handler+tsd only)",
                     (unsigned long long)n, (double)sum / (double)n);
        }
        if (g_trace) logf("<- %s = %#lx", g_thunks[swi].name.c_str(), (unsigned long)x(0));
    }

    // X86CODE_A64_SKIP_RET=1: when the handler did not redirect and the thunk is "svc; ret", return to X30 here
    // and skip the ret block. Outermost CallSVC only (a nested one runs inside call(), which restores the PC).
    // Reads X30 after the handler. Off by default: see ARM64-PLAN.md Speed 4.
    static const bool skip_ret = getenv("X86CODE_A64_SKIP_RET") && atoi(getenv("X86CODE_A64_SKIP_RET")) == 1;
    if (skip_ret && in_thunk < 0 && jit->GetPC() == pc0 && rd<u32>(pc0) == 0xD65F03C0u) jit->SetPC(x(30));
}

bool fp16_interpret(GuestThread &t, u32 insn);
void GuestThread::InterpreterFallback(u64 pc, size_t n) {
    // FEAT_FP16 SIMD (fp16.cpp): execute and continue after it
    u64 at = pc;
    for (size_t k = 0; k < n && fp16_interpret(*this, rd<uint32_t>(at)); k++) at += 4;
    if (at != pc) { jit->SetPC(at); return; }
    logf("unimplemented instruction %08x at %#lx lr %#lx sp %#lx x0 %#lx x16 %#lx x17 %#lx", rd<uint32_t>(pc), (unsigned long)pc,
         (unsigned long)x(30), (unsigned long)sp(), (unsigned long)x(0), (unsigned long)x(16), (unsigned long)x(17));
    if (FILE *m = fopen("/proc/self/maps", "r")) {
        char line[512];
        while (fgets(line, sizeof line, m)) {
            unsigned long lo, hi;
            if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && pc >= lo && pc < hi) logf("  in %s", line);
        }
        fclose(m);
    }
    abort();
}

// "file+offset" of a guest address outside the shared cache (from /proc/self/maps), for crash reports.
static std::mutex g_stub_syms_mu;
static std::map<std::string, std::vector<std::pair<u64, std::string>>> g_stub_syms;  // file name -> sorted (offset, export)
void register_stub_symbols(const std::string &file, std::vector<std::pair<u64, std::string>> fn) {
    std::sort(fn.begin(), fn.end());
    std::lock_guard<std::mutex> l(g_stub_syms_mu);
    g_stub_syms[file] = std::move(fn);
}
static std::string stub_symbol(const char *path, u64 off) {  // "/memfd:libsystem_kernel.dylib (deleted)"
    if (strncmp(path, "/memfd:", 7) != 0) return "";
    std::string file(path + 7);
    if (auto sp = file.find(" (deleted)"); sp != std::string::npos) file.resize(sp);
    std::lock_guard<std::mutex> l(g_stub_syms_mu);
    auto it = g_stub_syms.find(file);
    if (it == g_stub_syms.end()) return "";
    auto &v = it->second;
    auto u = std::upper_bound(v.begin(), v.end(), std::make_pair(off, std::string("\xff")));
    if (u == v.begin()) return "";
    --u;
    char b[300]; snprintf(b, sizeof b, " (%s+%#lx)", u->second.c_str(), (unsigned long)(off - u->first));
    return b;
}
std::string where(u64 a) {
    if (a >= 0x1c0000000ull && a < 0x2c0000000ull) return "cache";
    std::string r;
    if (FILE *m = fopen("/proc/self/maps", "r")) {
        char line[768];
        while (fgets(line, sizeof line, m)) {
            unsigned long lo, hi, off; char path[600] = "";
            if (sscanf(line, "%lx-%lx %*s %lx %*s %*s %599[^\n]", &lo, &hi, &off, path) >= 3 && a >= lo && a < hi) {
                char b[700]; snprintf(b, sizeof b, "%s+%#lx", path[0] ? path : "anon", a - lo + off); r = b;
                r += stub_symbol(path, a - lo + off);
                break;
            }
        }
        fclose(m);
    }
    return r;
}

void guest_backtrace(GuestThread &t) {
    u64 fp = t.x(29);
    logf("  lr %#lx %s", (unsigned long)t.x(30), where(t.x(30)).c_str());
    for (int k = 0; k < 24 && fp > 0x10000 && !(fp & 7); k++) {
        u64 ret = GuestThread::rd<u64>(fp + 8);
        logf("  #%d %#lx %s", k, (unsigned long)ret, where(ret).c_str());
        u64 next = GuestThread::rd<u64>(fp);
        if (next <= fp) break;
        fp = next;
    }
}

void GuestThread::InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, u64 value) {
    // Guest IC IVAU / IALLU after writing instructions (dyld, objc, fishhook). Default dynarmic callback is a
    // no-op; per-thread 8 MB caches masked that by evacuating on full, but the shared cache never frees.
    using Op = Dynarmic::A64::InstructionCacheOperation;
    if (!jit) return;
    static const bool iclog = getenv("X86CODE_A64_ICLOG") != nullptr;
    static const bool ivau_clear = getenv("X86CODE_A64_IVAU_CLEAR") && atoi(getenv("X86CODE_A64_IVAU_CLEAR")) == 1;
    if (iclog) {
        const char *name = "?";
        switch (op) {
        case Op::InvalidateByVAToPoU: name = "IVAU"; break;
        case Op::InvalidateAllToPoU: name = "IALLU"; break;
        case Op::InvalidateAllToPoUInnerSharable: name = "IALLUIS"; break;
        }
        logf("IC %s value=%#lx", name, (unsigned long)value);
    }
    switch (op) {
    case Op::InvalidateByVAToPoU:
        // X86CODE_A64_IVAU_CLEAR=1: treat every line invalidate as a full ClearCache (bisect SMC races).
        if (ivau_clear) {
            jit->ClearCache();
        } else {
            // One cache line; invalidate a 64-byte window covering the VA.
            jit->InvalidateCacheRange(value & ~u64(63), 64);
        }
        break;
    case Op::InvalidateAllToPoU:
    case Op::InvalidateAllToPoUInnerSharable:
        jit->ClearCache();
        break;
    }
}

void GuestThread::ExceptionRaised(u64 pc, Dynarmic::A64::Exception e) {
    using E = Dynarmic::A64::Exception;
    // hints (spin loops: YIELD, WFE in os_unfair_lock/dispatch): continue at the next instruction
    if (e == E::Yield || e == E::WaitForEvent || e == E::WaitForInterrupt) { sched_yield(); return; }
    if (e == E::SendEvent || e == E::SendEventLocal) return;
    uint32_t insn = rd<uint32_t>(pc);
    // FEAT_FP16 scalar GPR<->half (SCVTF/UCVTF h,w): dynarmic raises UnallocatedEncoding; interpret and continue.
    if (e == E::UnallocatedEncoding && fp16_interpret(*this, insn)) {
        jit->SetPC(pc + 4);
        return;
    }
    // brk #0xf... (Swift/clang traps), udf: report like a crash with the guest registers.
    logf("guest exception %d at pc %#lx %s (insn %08x) lr %#lx sp %#lx", (int)e, (unsigned long)pc, where(pc).c_str(), insn,
         (unsigned long)x(30), (unsigned long)sp());
    for (int i = 0; i < 31; i += 4)
        logf("  x%-2d %016lx x%-2d %016lx x%-2d %016lx x%-2d %016lx", i, (unsigned long)x(i), i + 1,
             (unsigned long)(i + 1 < 31 ? x(i + 1) : 0), i + 2, (unsigned long)(i + 2 < 31 ? x(i + 2) : 0), i + 3,
             (unsigned long)(i + 3 < 31 ? x(i + 3) : 0));
    guest_backtrace(*this);
    abort();
}

u64 GuestThread::GetCNTPCT() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    const u64 ns = ts.tv_sec * 1000000000ull + ts.tv_nsec;
    return timebase_apple() ? tb_ns_to_ticks(ns) : ns;
}

bool g_trace;
void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("arm64host: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    abort();
}
thread_local bool g_non_darling_thread = false;
void logf(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    if (write(2, "arm64host: ", 11) < 0 || write(2, buf, n) < 0) {}
    // X86CODE_A64_LOG=<Darwin dir>: also append to <dir>/<exe>.<pid>.log (simulator jobs' stderr is often gone)
    // (opened for every line: daemons close all fds at start, a kept fd would end up in another file)
    // a char array, not std::string: lines logged during exit (after static destructors) went to a file named by
    // the freed string's bytes in the cwd
    static char path[512];
    static bool init;
    if (!init) {
        init = true;
        if (const char *dir = getenv("X86CODE_A64_LOG")) {
            static auto *dmkdir = (int (*)(const char *, int))dsym("mkdir");
            const char *exe = getenv("X86CODE_A64_EXE");
            const char *b = exe ? strrchr(exe, '/') : nullptr;
            if (dmkdir) dmkdir(dir, 0777);
            snprintf(path, sizeof path, "%s/%s.%d.log", dir, b ? b + 1 : "guest", (int)getpid());
        }
    }
    if (path[0] && !g_non_darling_thread) {  // Darling's open/write abort on a thread Darling does not know
        static auto *dopen = (int (*)(const char *, int, ...))dsym("open");
        static auto *dwrite = (long (*)(int, const void *, size_t))dsym("write");
        static auto *dclose = (int (*)(int))dsym("close");
        int fd = dopen ? dopen(path, 0x1000209 /* O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC */, 0644) : -1;
        if (fd >= 0) { dwrite(fd, buf, n); dclose(fd); }
    }
}

// Native -> guest entry (from a64_tramp_entry). Frame: ints[8], fps[8], result[4].
extern "C" void a64_enter_guest(u64 pc, u64 *frame) {
    GuestThread &t = GuestThread::self();
    t.tsd_from_native();
    NRet r = t.call(pc, frame, 8, frame + 8, 8);
    t.tsd_to_native();
    frame[16] = r.rax; frame[17] = r.rdx; frame[18] = r.xmm0; frame[19] = r.xmm1;
}

}  // namespace a64
