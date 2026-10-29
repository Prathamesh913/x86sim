// arm64run: run an arm64 iOS-simulator command-line Mach-O on x86_64 Linux under dynarmic (stage 0 of ARM64-PLAN.md).
// Usage: arm64run <binary> [args...]   ARM64RUN_STATS=1 prints timing and boundary counters to stderr.
#include "arm64run.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/mman.h>
#include <unistd.h>

namespace a64r {

thread_local GuestThread *current;
Dynarmic::ExclusiveMonitor *g_monitor;
static thread_local uint64_t tls_tpidr;
static std::atomic<uint64_t> g_svc_total, g_lse_total;

GuestThread::GuestThread(size_t cpu) {
    Dynarmic::A64::UserConfig c;
    c.callbacks = this;
    c.processor_id = cpu;
    c.global_monitor = g_monitor;
    c.fastmem_pointer = 0;                 // identity mapping: guest address == host address
    c.fastmem_address_space_bits = 47;     // x86-64 user space
    c.silently_mirror_fastmem = false;
    c.fastmem_exclusive_access = true;
    c.enable_cycle_counting = false;
    c.wall_clock_cntpct = true;
    c.cntfrq_el0 = 24000000;               // Apple's 24 MHz counter
    c.tpidr_el0 = &tls_tpidr;
    c.code_cache_size = 64u << 20;
    if (getenv("ARM64RUN_UNSAFE")) {       // dynarmic's unsafe FP shortcuts (NaN payloads, FPCR assumed default)
        c.unsafe_optimizations = true;
        c.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN | Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue |
                           Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;
    }
    jit = std::make_unique<Dynarmic::A64::Jit>(c);
    stack_size = 8u << 20;
    stack_base = (uint64_t)mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
}
GuestThread::~GuestThread() { g_svc_total += svc_count; g_lse_total += lse_count; munmap((void *)stack_base, stack_size); }

uint64_t GuestThread::call(uint64_t pc, std::initializer_list<uint64_t> args) {
    current = this;
    int i = 0;
    for (auto a : args) jit->SetRegister(i++, a);
    jit->SetRegister(30, g_exit_stub);
    jit->SetSP((stack_base + stack_size - 64) & ~15ull);
    jit->SetPC(pc);
    exited = false;
    while (!exited) jit->Run();
    return jit->GetRegister(0);
}

double GuestThread::d(int i) { double v; uint64_t b = jit->GetVector(i)[0]; memcpy(&v, &b, 8); return v; }
void GuestThread::set_d0(double v) { uint64_t b; memcpy(&b, &v, 8); jit->SetVector(0, {b, 0}); }

bool GuestThread::MemoryWriteExclusive128(uint64_t a, Dynarmic::A64::Vector v, Dynarmic::A64::Vector e) {
    unsigned __int128 ev = ((unsigned __int128)e[1] << 64) | e[0], nv = ((unsigned __int128)v[1] << 64) | v[0];
    return __atomic_compare_exchange_n((unsigned __int128 *)a, &ev, nv, false, 5, 5);
}

void GuestThread::CallSVC(uint32_t swi) {
    if (swi == kExitSvc) { exited = true; jit->HaltExecution(); return; }
    svc_count++;
    g_thunks[swi].fn(*this);
}

void GuestThread::InterpreterFallback(uint64_t pc, size_t) {
    uint32_t insn = rd<uint32_t>(pc);
    if (emulate_lse(*this, pc, insn)) { lse_count++; return; }
    fprintf(stderr, "arm64run: unimplemented instruction %08x at %#lx\n", insn, (unsigned long)pc);
    abort();
}
void GuestThread::ExceptionRaised(uint64_t pc, Dynarmic::A64::Exception e) {
    fprintf(stderr, "arm64run: guest exception %d at %#lx (insn %08x)\n", (int)e, (unsigned long)pc, rd<uint32_t>(pc));
    abort();
}
uint64_t GuestThread::GetCNTPCT() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (ts.tv_sec * 1000000000ull + ts.tv_nsec) * 3 / 125; }

}  // namespace a64r

using namespace a64r;
static double ms(timespec a, timespec b) { return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6; }

int main(int argc, char **argv, char **envp) {
    if (argc < 2) { fprintf(stderr, "usage: arm64run <arm64 Mach-O> [args...]\n"); return 2; }
    timespec t0, t1, t2, t3; clock_gettime(CLOCK_MONOTONIC, &t0);
    bool stats = getenv("ARM64RUN_STATS");
    g_monitor = new Dynarmic::ExclusiveMonitor(64);
    thunks_init();
    Image img = load_macho(argv[1]);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    static const char *apple[] = {"executable_path=", nullptr};
    int code;
    {
        GuestThread main_thread(0);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        code = (int)main_thread.call(img.entry, {(uint64_t)(argc - 1), (uint64_t)(argv + 1), (uint64_t)envp, (uint64_t)apple});
    }
    fflush(stdout);
    clock_gettime(CLOCK_MONOTONIC, &t3);
    if (stats)
        fprintf(stderr, "arm64run: load %.2f ms, jit init %.2f ms, run %.2f ms; thunk calls %lu, emulated LSE %lu, imports %zu\n",
                ms(t0, t1), ms(t1, t2), ms(t2, t3), (unsigned long)g_svc_total, (unsigned long)g_lse_total, g_thunks.size());
    return code;
}
