// arm64run: the native x86 side of the boundary. Every imported function is bound to a stub "svc #i; ret" in the
// thunk page; dynarmic's CallSVC callback runs native handler i with the guest's registers (Apple arm64 ABI in,
// SysV x86-64 call out), puts the result in x0/d0, and the guest returns through the "ret". The simulator runtime
// itself never executes svc (its syscalls live in libsystem_kernel, which we supply), so svc is free for the boundary.
// Stage 0 implements a minimal libSystem directly on glibc; stage 1 replaces it with Darling's x86 libsystem_kernel/
// platform/pthread.
#include "arm64run.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

namespace a64r {

std::vector<Thunk> g_thunks;
uint64_t g_exit_stub;
static uint32_t *g_stub_page;
static constexpr size_t kMaxThunks = 8192;
static uint64_t g_stack_chk_guard = 0x2d5e0a1b9c7f3a11ull;
static std::map<std::string, uint64_t> g_bound;

using u64 = uint64_t;
using fn6 = u64 (*)(u64, u64, u64, u64, u64, u64);

// Integer/pointer-only C functions with no struct-layout differences: call straight through.
template <auto F> static void pass(GuestThread &t) {
    t.set_x(0, ((fn6)(void *)F)(t.x(0), t.x(1), t.x(2), t.x(3), t.x(4), t.x(5)));
}

// printf family. Apple arm64 passes variadic arguments on the stack in 8-byte slots (doubles too), and va_list is a
// plain pointer to those slots, so one formatter serves printf/vprintf/snprintf/... by walking the slots.
static std::string guest_format(const char *f, const u64 *slot) {
    std::string out; char spec[64], buf[512];
    while (*f) {
        if (*f != '%') { out += *f++; continue; }
        if (f[1] == '%') { out += '%'; f += 2; continue; }
        size_t n = 0; spec[n++] = *f++;
        int star_w = -1, star_p = -1;
        while (*f && strchr("-+ #0", *f) && n < 40) spec[n++] = *f++;
        if (*f == '*') { star_w = (int)*slot++; spec[n++] = *f++; }
        while (*f >= '0' && *f <= '9' && n < 40) spec[n++] = *f++;
        if (*f == '.') { spec[n++] = *f++; if (*f == '*') { star_p = (int)*slot++; spec[n++] = *f++; }
                         while (*f >= '0' && *f <= '9' && n < 40) spec[n++] = *f++; }
        bool longlong = false;
        while (*f && strchr("hlqjztL", *f) && n < 50) { if (*f != 'h') longlong = true; spec[n++] = *f++; }
        char c = *f ? *f++ : 0; spec[n++] = c; spec[n] = 0;
        u64 v = *slot++;
        auto emit = [&](auto arg) {
            if (star_w >= 0 && star_p >= 0) snprintf(buf, sizeof buf, spec, star_w, star_p, arg);
            else if (star_w >= 0) snprintf(buf, sizeof buf, spec, star_w, arg);
            else if (star_p >= 0) snprintf(buf, sizeof buf, spec, star_p, arg);
            else snprintf(buf, sizeof buf, spec, arg);
            out += buf;
        };
        switch (c) {
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d; memcpy(&d, &v, 8); if (spec[n - 2] == 'L') spec[n - 2] = 'l'; emit(d); break; }
        case 's': emit(v ? (const char *)v : "(null)"); break;
        case 'p': emit((void *)v); break;
        case 'c': emit((int)v); break;
        default: if (longlong) emit((long long)v); else emit((int)v); break;
        }
    }
    return out;
}
static void h_printf(GuestThread &t) { auto s = guest_format((const char *)t.x(0), (const u64 *)t.sp()); fwrite(s.data(), 1, s.size(), stdout); t.set_x(0, s.size()); }
static void h_vprintf(GuestThread &t) { auto s = guest_format((const char *)t.x(0), (const u64 *)t.x(1)); fwrite(s.data(), 1, s.size(), stdout); t.set_x(0, s.size()); }
static void h_snprintf(GuestThread &t) {
    auto s = guest_format((const char *)t.x(2), (const u64 *)t.sp());
    if (t.x(1)) { size_t m = std::min<size_t>(s.size(), t.x(1) - 1); memcpy((void *)t.x(0), s.data(), m); ((char *)t.x(0))[m] = 0; }
    t.set_x(0, s.size());
}
static void h_vsnprintf(GuestThread &t) {
    auto s = guest_format((const char *)t.x(2), (const u64 *)t.x(3));
    if (t.x(1)) { size_t m = std::min<size_t>(s.size(), t.x(1) - 1); memcpy((void *)t.x(0), s.data(), m); ((char *)t.x(0))[m] = 0; }
    t.set_x(0, s.size());
}
static void h_exit(GuestThread &t) { fflush(nullptr); _exit((int)t.x(0)); }
static void h_stack_chk_fail(GuestThread &) { fprintf(stderr, "arm64run: guest stack smashing detected\n"); abort(); }

// mach time: report nanoseconds with a 1/1 timebase.
static u64 now_ns() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ull + ts.tv_nsec; }
static void h_mach_absolute_time(GuestThread &t) { t.set_x(0, now_ns()); }
static void h_mach_timebase_info(GuestThread &t) { auto *p = (uint32_t *)t.x(0); p[0] = 1; p[1] = 1; t.set_x(0, 0); }

// pthread. Darwin's pthread_mutex_t is 64 bytes {long sig; char opaque[56]} and its static initializer is
// sig 0x32AAABA7, so a glibc mutex (40 bytes) lives at offset 8, initialized on first use.
static constexpr long kDarwinMutexInit = 0x32AAABA7, kBusy = 1, kReady = 0x58383658;
static pthread_mutex_t *host_mutex(u64 m) {
    auto *sig = (long *)m;
    long s = __atomic_load_n(sig, __ATOMIC_ACQUIRE);
    if (s != kReady) {
        if (s != kBusy && __atomic_compare_exchange_n(sig, &s, kBusy, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            pthread_mutex_init((pthread_mutex_t *)(m + 8), nullptr);
            __atomic_store_n(sig, kReady, __ATOMIC_RELEASE);
        } else while (__atomic_load_n(sig, __ATOMIC_ACQUIRE) != kReady) sched_yield();
    }
    return (pthread_mutex_t *)(m + 8);
}
static void h_mutex_init(GuestThread &t) { *(long *)t.x(0) = kDarwinMutexInit; host_mutex(t.x(0)); t.set_x(0, 0); }
static void h_mutex_lock(GuestThread &t) { t.set_x(0, pthread_mutex_lock(host_mutex(t.x(0)))); }
static void h_mutex_unlock(GuestThread &t) { t.set_x(0, pthread_mutex_unlock(host_mutex(t.x(0)))); }
static void h_mutex_trylock(GuestThread &t) { t.set_x(0, pthread_mutex_trylock(host_mutex(t.x(0)))); }
static void h_mutex_destroy(GuestThread &t) { t.set_x(0, 0); }

// pthread_create: a host thread with its own Jit runs the guest start routine (one Jit per guest thread).
struct Start { u64 fn, arg; };
static std::atomic<size_t> g_next_cpu{1};
static void *thread_main(void *p) {
    Start s = *(Start *)p; delete (Start *)p;
    GuestThread t(g_next_cpu++ % 64);
    return (void *)t.call(s.fn, {s.arg});
}
static void h_pthread_create(GuestThread &t) {
    pthread_t th;
    int r = pthread_create(&th, nullptr, thread_main, new Start{t.x(2), t.x(3)});
    if (!r) *(u64 *)t.x(0) = (u64)th;
    t.set_x(0, r);
}
static void h_pthread_join(GuestThread &t) { t.set_x(0, pthread_join((pthread_t)t.x(0), (void **)t.x(1))); }
static void h_pthread_self(GuestThread &t) { t.set_x(0, (u64)pthread_self()); }

static void h_missing(GuestThread &t) {
    // find which stub we came from: the svc is the instruction before the current PC
    uint32_t insn = *(uint32_t *)(t.jit->GetPC() - 4);
    fprintf(stderr, "arm64run: unimplemented import %s\n", g_thunks[(insn >> 5) & 0xffff].name.c_str());
    abort();
}

static const std::map<std::string, Handler> &table() {
    static const std::map<std::string, Handler> t = {
        {"_printf", h_printf}, {"_vprintf", h_vprintf}, {"_snprintf", h_snprintf}, {"_vsnprintf", h_vsnprintf},
        {"_puts", pass<puts>}, {"_putchar", pass<putchar>}, {"_strlen", pass<strlen>}, {"_strcmp", pass<strcmp>},
        {"_malloc", pass<malloc>}, {"_calloc", pass<calloc>}, {"_realloc", pass<realloc>}, {"_free", pass<free>},
        {"_memcpy", pass<memcpy>}, {"_memmove", pass<memmove>}, {"_memset", pass<memset>}, {"_memcmp", pass<memcmp>},
        {"_bzero", pass<bzero>}, {"_atoi", pass<atoi>}, {"_write", pass<write>},
        {"_exit", h_exit}, {"___stack_chk_fail", h_stack_chk_fail}, {"_abort", pass<abort>},
        {"_mach_absolute_time", h_mach_absolute_time}, {"_mach_timebase_info", h_mach_timebase_info},
        {"_pthread_create", h_pthread_create}, {"_pthread_join", h_pthread_join}, {"_pthread_self", h_pthread_self},
        {"_pthread_mutex_init", h_mutex_init}, {"_pthread_mutex_lock", h_mutex_lock},
        {"_pthread_mutex_unlock", h_mutex_unlock}, {"_pthread_mutex_trylock", h_mutex_trylock},
        {"_pthread_mutex_destroy", h_mutex_destroy},
    };
    return t;
}

void thunks_init() {
    g_stub_page = (uint32_t *)mmap(nullptr, kMaxThunks * 8 + 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_exit_stub = (u64)&g_stub_page[kMaxThunks * 2];
    g_stub_page[kMaxThunks * 2] = 0xD4000001u | (kExitSvc << 5);
}

uint64_t thunk_for(const std::string &sym) {
    if (sym == "___stack_chk_guard") return (u64)&g_stack_chk_guard;
    if (auto it = g_bound.find(sym); it != g_bound.end()) return it->second;
    size_t i = g_thunks.size();
    if (i >= kMaxThunks) { fprintf(stderr, "arm64run: too many imports\n"); exit(127); }
    auto it = table().find(sym);
    g_thunks.push_back({sym, it == table().end() ? h_missing : it->second});
    g_stub_page[2 * i] = 0xD4000001u | ((uint32_t)i << 5);  // svc #i
    g_stub_page[2 * i + 1] = 0xD65F03C0u;                    // ret
    return g_bound[sym] = (u64)&g_stub_page[2 * i];
}

}  // namespace a64r
