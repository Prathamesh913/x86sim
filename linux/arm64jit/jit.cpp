// libarm64jit: guest threads. Each native thread that runs guest code gets one dynarmic Jit and an 8 MB guest stack.
// Native code may call back into guest code on the same thread (qsort comparators, pthread_once, TSD destructors...):
// call() then runs a nested Jit::Run (re-entrant in our dynarmic fork) on a stack frame 128 bytes below the guest's
// current SP and restores the full guest state afterwards.
#include "a64.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/mman.h>
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace a64 {

static Dynarmic::ExclusiveMonitor *monitor() {
    static auto *m = new Dynarmic::ExclusiveMonitor(64);
    return m;
}
static std::atomic<size_t> g_next_cpu{0};
static thread_local GuestThread *t_self;
static thread_local struct Owner { GuestThread *t = nullptr; ~Owner() { delete t; } } t_owner;

GuestThread &GuestThread::self() {
    if (!t_self) { t_owner.t = new GuestThread(); t_self = t_owner.t; }
    return *t_self;
}

GuestThread *GuestThread::current() { return t_self; }

GuestThread::GuestThread() {
    // Darwin's TSD base is the gs base on x86_64 (slot 0 = pthread_self). Guest tpidrro_el0 reads see it.
    unsigned long gs = 0;
    syscall(SYS_arch_prctl, ARCH_GET_GS, &gs);
    tpidrro = gs;
    Dynarmic::A64::UserConfig c;
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
    c.code_cache_size = 64u << 20;
    jit = std::make_unique<Dynarmic::A64::Jit>(c);
    stack_size = 8u << 20;
    stack_base = (u64)mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (stack_base == (u64)MAP_FAILED) die("cannot allocate a guest stack");
}
GuestThread::~GuestThread() { munmap((void *)stack_base, stack_size); }

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
    static const bool step = getenv("X86CODE_A64_STEP");  // debugging: one instruction per run, exact fault PCs
    if (step) while (!finished) jit->Step();
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
    return __atomic_compare_exchange_n((unsigned __int128 *)a, &ev, nv, false, 5, 5);
}

bool g_count_thunks = getenv("X86CODE_A64_STATS") != nullptr;
u64 g_thunk_counts[0x10000];

void GuestThread::CallSVC(uint32_t swi) {
    if (swi == kExitSvc) { *done = true; jit->HaltExecution(); return; }
    svc_count++;
    if (swi >= g_thunks.size()) die("svc #%u at %#lx: no such thunk", swi, (unsigned long)jit->GetPC() - 4);
    if (g_count_thunks) __atomic_fetch_add(&g_thunk_counts[swi], 1, __ATOMIC_RELAXED);
    if (g_trace) logf("-> %s(%#lx, %#lx, %#lx)", g_thunks[swi].name.c_str(), (unsigned long)x(0), (unsigned long)x(1), (unsigned long)x(2));
    g_thunks[swi].fn(*this, swi);
    if (g_trace) logf("<- %s = %#lx", g_thunks[swi].name.c_str(), (unsigned long)x(0));
}

void GuestThread::InterpreterFallback(u64 pc, size_t) {
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

void GuestThread::ExceptionRaised(u64 pc, Dynarmic::A64::Exception e) {
    uint32_t insn = rd<uint32_t>(pc);
    // brk #0xf... (Swift/clang traps), udf: report like a crash with the guest registers.
    fprintf(stderr, "arm64host: guest exception %d at pc %#lx (insn %08x) lr %#lx sp %#lx\n", (int)e, (unsigned long)pc, insn,
            (unsigned long)x(30), (unsigned long)sp());
    for (int i = 0; i < 31; i += 4)
        fprintf(stderr, "  x%-2d %016lx x%-2d %016lx x%-2d %016lx x%-2d %016lx\n", i, (unsigned long)x(i), i + 1,
                (unsigned long)(i + 1 < 31 ? x(i + 1) : 0), i + 2, (unsigned long)(i + 2 < 31 ? x(i + 2) : 0), i + 3,
                (unsigned long)(i + 3 < 31 ? x(i + 3) : 0));
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
}

// Native -> guest entry (from a64_tramp_entry). Frame: ints[8], fps[8], result[4].
extern "C" void a64_enter_guest(u64 pc, u64 *frame) {
    NRet r = GuestThread::self().call(pc, frame, 8, frame + 8, 8);
    frame[16] = r.rax; frame[17] = r.rdx; frame[18] = r.xmm0; frame[19] = r.xmm1;
}

}  // namespace a64
