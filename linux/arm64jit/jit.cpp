// libarm64jit: guest threads. Each native thread that runs guest code gets one dynarmic Jit and an 8 MB guest stack.
// Native code may call back into guest code on the same thread (qsort comparators, pthread_once, TSD destructors...):
// call() then runs a nested Jit::Run (re-entrant in our dynarmic fork) on a stack frame 128 bytes below the guest's
// current SP and restores the full guest state afterwards.
#include "a64.h"
#include <atomic>
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
static thread_local GuestThread *t_self;
static thread_local struct Owner { GuestThread *t = nullptr; ~Owner() { delete t; } } t_owner;

// Darling's threads end without running the ELF's thread_local destructors: a native (Darling) TSD key with a
// destructor frees the thread's GuestThread (Jit, code cache, guest stack) when the thread exits. Before this every
// exited workqueue thread leaked ~20 MB.
static void destroy_guest_thread(void *p) {
    auto *t = (GuestThread *)p;
    if (t->depth == 0 && t_self == t) { t_self = nullptr; t_owner.t = nullptr; delete t; }
}
GuestThread &GuestThread::self() {
    if (!t_self) {
        t_owner.t = new GuestThread(); t_self = t_owner.t;
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

GuestThread::GuestThread() {
    // The guest has its own TSD (tpidrro_el0): slots 0-9 (pthread_self, errno, mach thread self, ...) are copied from
    // Darling's native TSD (the gs base on x86_64); keys 10+ are the guest's alone. Sharing the native array let
    // Darling's own libobjc/libdispatch destructors free guest values at thread exit (SIGABRT in every daemon).
    // Guest TSD destructors are not run at thread exit (leak).
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
    c.cntfrq_el0 = 1000000000;           // the counter counts ns, like Darling's mach_absolute_time (timebase 1/1)
    c.tpidr_el0 = &tpidr;
    c.tpidrro_el0 = &tpidrro;
    // x86Code: every guest thread has its own Jit (translations are not shared); a full cache is cleared, which also
    // drops its block metadata (in glibc malloc, ~3x the code). 16 MB bounds a thread at ~60 MB.
    c.code_cache_size = (getenv("X86CODE_A64_CODECACHE_MB") ? atoi(getenv("X86CODE_A64_CODECACHE_MB")) : 16) << 20;
    static const bool rss_debug = getenv("X86CODE_A64_RSS") != nullptr;
    auto rss = [] { long v = 0; if (FILE *f = fopen("/proc/self/statm", "r")) { long a, b, c2; if (fscanf(f, "%ld %ld %ld", &a, &b, &c2) == 3) v = (b - c2) * 4; fclose(f); } return v; };
    long before = rss_debug ? rss() : 0;
    jit = std::make_unique<Dynarmic::A64::Jit>(c);
    if (rss_debug) { struct mallinfo2 mi = mallinfo2(); logf("new Jit: anon RSS +%ld kB; glibc mmapped %zu MB in %zu blocks, arena %zu MB", rss() - before, mi.hblkhd >> 20, mi.hblks, mi.arena >> 20); }
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
    jit = std::make_unique<Dynarmic::A64::Jit>(cfg);
}
GuestThread::~GuestThread() { munmap((void *)stack_base, stack_size); free(tsd); }

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
    if (g_step && depth == 1) while (!finished) jit->Step();
    else while (!finished) { jit->Run(); if (sig_pending.load(std::memory_order_relaxed)) deliver_signals(*this); }
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

void GuestThread::CallSVC(uint32_t swi) {
    if (swi == kExitSvc) { *done = true; jit->HaltExecution(); return; }
    if (swi == kLinuxSvc) {  // our in-simulator code's escape to a Linux syscall (x8 = x86_64 number, x0-x5 args)
        set_x(0, (u64)syscall((long)x(8), x(0), x(1), x(2), x(3), x(4), x(5)));
        return;
    }
    svc_count++;
    if (swi >= g_thunks.size()) die("svc #%u at %#lx: no such thunk", swi, (unsigned long)jit->GetPC() - 4);
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
    int32_t outer_thunk = in_thunk; in_thunk = (int32_t)swi;
    tsd_to_native();
    g_thunks[swi].fn(*this, swi);
    tsd_from_native();
    in_thunk = outer_thunk;
    if (g_trace) logf("<- %s = %#lx", g_thunks[swi].name.c_str(), (unsigned long)x(0));
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
std::string where(u64 a) {
    if (a >= 0x1c0000000ull && a < 0x2c0000000ull) return "cache";
    std::string r;
    if (FILE *m = fopen("/proc/self/maps", "r")) {
        char line[768];
        while (fgets(line, sizeof line, m)) {
            unsigned long lo, hi, off; char path[600] = "";
            if (sscanf(line, "%lx-%lx %*s %lx %*s %*s %599[^\n]", &lo, &hi, &off, path) >= 3 && a >= lo && a < hi) {
                char b[700]; snprintf(b, sizeof b, "%s+%#lx", path[0] ? path : "anon", a - lo + off); r = b; break;
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

void GuestThread::ExceptionRaised(u64 pc, Dynarmic::A64::Exception e) {
    using E = Dynarmic::A64::Exception;
    // hints (spin loops: YIELD, WFE in os_unfair_lock/dispatch): continue at the next instruction
    if (e == E::Yield || e == E::WaitForEvent || e == E::WaitForInterrupt) { sched_yield(); return; }
    if (e == E::SendEvent || e == E::SendEventLocal) return;
    uint32_t insn = rd<uint32_t>(pc);
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
    return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

bool g_trace;
void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("arm64host: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    abort();
}
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
    static std::string path;
    static bool init;
    if (!init) {
        init = true;
        if (const char *dir = getenv("X86CODE_A64_LOG")) {
            static auto *dmkdir = (int (*)(const char *, int))dsym("mkdir");
            const char *exe = getenv("X86CODE_A64_EXE");
            const char *b = exe ? strrchr(exe, '/') : nullptr;
            char p[512];
            if (dmkdir) dmkdir(dir, 0777);
            snprintf(p, sizeof p, "%s/%s.%d.log", dir, b ? b + 1 : "guest", (int)getpid());
            path = p;
        }
    }
    if (!path.empty()) {
        static auto *dopen = (int (*)(const char *, int, ...))dsym("open");
        static auto *dwrite = (long (*)(int, const void *, size_t))dsym("write");
        static auto *dclose = (int (*)(int))dsym("close");
        int fd = dopen ? dopen(path.c_str(), 0x1000209 /* O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC */, 0644) : -1;
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
