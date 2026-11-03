// libarm64jit: guest signal handlers (ARM64-PLAN stage 2, prerequisite 1b).
//
// __sigaction installs one native handler for every signal the guest handles. When it runs (from Darling's
// _sigtramp, on whatever the thread was doing) it only records the signal in the thread's GuestThread and halts the
// thread's Jit; dynarmic stops at the next block boundary (or when the current svc handler returns to guest code),
// GuestThread::call() then delivers: it builds an arm64 Darwin ucontext (mcontext64: exception, thread and NEON
// state) on the guest stack, blocks sa_mask + the signal, calls the guest handler with (sig, siginfo, ucontext), and
// resumes from the (possibly modified) thread state. Signals on threads that run no guest code are dropped with a log.
// Synchronous faults of guest code (SIGSEGV/SIGBUS/SIGILL/SIGTRAP from the JIT itself) are not routed here.
#include "a64.h"
#include <atomic>
#include <csignal>
#include <cerrno>
#include <mutex>
#include <array>
#include <ctime>
#include <link.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace a64 {

namespace {
struct GuestAction { std::atomic<u64> handler{0}; std::atomic<uint32_t> mask{0}; std::atomic<int> flags{0}; };
GuestAction g_act[32];
constexpr int kSA_SIGINFO = 0x40, kSA_RESETHAND = 0x4, kSA_NODEFER = 0x10;  // Darwin values
constexpr int kSIG_BLOCK = 1, kSIG_SETMASK = 3;

// Darwin arm64 ucontext + mcontext64.
struct ArmUcontext {
    int32_t uc_onstack; uint32_t uc_sigmask;
    u64 ss_sp, ss_size; int32_t ss_flags, pad0;
    u64 uc_link, uc_mcsize, uc_mcontext;
};
struct ArmMcontext {
    u64 far; uint32_t esr, exception;                // __darwin_arm_exception_state64
    u64 x[29], fp, lr, sp, pc; uint32_t cpsr, pad;   // __darwin_arm_thread_state64
    Dynarmic::A64::Vector v[32]; uint32_t fpsr, fpcr; // __darwin_arm_neon_state64
    u64 pad2;
};
static_assert(sizeof(ArmUcontext) == 56 && offsetof(ArmMcontext, x) == 16 && offsetof(ArmMcontext, v) == 288 && sizeof(ArmMcontext) == 816);

void native_handler(int sig, void *info, void *) {
    GuestThread *t = GuestThread::current();
    if (!t || !t->jit || sig <= 0 || sig >= 32) {
        static const char m[] = "arm64host: signal on a thread without guest code: dropped\n";
        if (write(2, m, sizeof m - 1) < 0) {}
        return;
    }
    if (info) memcpy(t->sig_info[sig], info, sizeof t->sig_info[sig]);
    else memset(t->sig_info[sig], 0, sizeof t->sig_info[sig]);
    t->sig_pending.fetch_or(1u << sig);
    t->jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
}
}  // namespace

// __sigaction(int, struct __sigaction *{handler, tramp, mask, flags}, struct sigaction *{handler, mask, flags}) and
// __platform_sigaction(int, struct sigaction *, struct sigaction *) (what iOS 27's libc sigaction() calls).
struct NSA { u64 handler, tramp; uint32_t mask; int flags; };
struct OSA { u64 handler; uint32_t mask; int flags; };
template <bool Kernel> static void sigaction_like(GuestThread &t, u32 i) {
    int sig = (int)t.x(0);
    OSA *osa = (OSA *)t.x(2);
    OSA g{};  // the guest's request
    bool set = t.x(1) != 0;
    if (set) {
        if (Kernel) { NSA *n = (NSA *)t.x(1); g = {n->handler, n->mask, n->flags}; }
        else g = *(OSA *)t.x(1);
    }
    u64 nh = g.handler; int nf = g.flags;
    if (set && sig > 0 && sig < 32 && g.handler > 1) { nh = (u64)&native_handler; nf |= kSA_SIGINFO; }  // not DFL/IGN
    u64 old_guest = sig > 0 && sig < 32 ? g_act[sig].handler.load() : 0;
    int old_flags = sig > 0 && sig < 32 ? g_act[sig].flags.load() : 0;
    int r;
    if (Kernel) {
        static u64 tramp = (u64)dsym("_sigtramp");
        NSA n{nh, tramp ? tramp : ((NSA *)t.x(1) ? ((NSA *)t.x(1))->tramp : 0), g.mask, nf};
        r = ((int (*)(int, NSA *, OSA *))g_thunks[i].native)(sig, set ? &n : nullptr, osa);
    } else {
        OSA n{nh, g.mask, nf};
        r = ((int (*)(int, OSA *, OSA *))g_thunks[i].native)(sig, set ? &n : nullptr, osa);
    }
    if (r == 0 && osa && osa->handler == (u64)&native_handler) { osa->handler = old_guest; osa->flags = old_flags; }
    if (r == 0 && set && sig > 0 && sig < 32) { g_act[sig].handler = g.handler; g_act[sig].mask = g.mask; g_act[sig].flags = g.flags; }
    t.set_x(0, (u32)r);
}
void h_sigaction(GuestThread &t, u32 i) { sigaction_like<true>(t, i); }
void h_platform_sigaction(GuestThread &t, u32 i) { sigaction_like<false>(t, i); }

// Called by GuestThread::call() when Run() returned with signals pending; the guest state is at a block boundary.
void deliver_signals(GuestThread &t) {
    static auto *dsigmask = (int (*)(int, const uint32_t *, uint32_t *))dsym("pthread_sigmask");
    while (uint32_t pend = t.sig_pending.load()) {
        int sig = __builtin_ctz(pend);
        t.sig_pending.fetch_and(~(1u << sig));
        u64 h = g_act[sig].handler.load();
        int flags = g_act[sig].flags.load();
        if (g_trace) logf("signal %d -> guest handler %#lx flags %#x", sig, (unsigned long)h, flags);
        if (h <= 1) continue;  // reset meanwhile
        if (flags & kSA_RESETHAND) g_act[sig].handler = 0;
        auto &j = *t.jit;
        // frame on the guest stack below the red zone: siginfo, ucontext, mcontext
        u64 sp = (j.GetSP() - 128 - sizeof(ArmMcontext) - sizeof(ArmUcontext) - 128) & ~15ull;
        auto *mc = (ArmMcontext *)sp;
        auto *uc = (ArmUcontext *)(sp + sizeof(ArmMcontext));
        auto *si = (uint8_t *)(sp + sizeof(ArmMcontext) + sizeof(ArmUcontext));
        memset(mc, 0, sizeof *mc); memset(uc, 0, sizeof *uc);
        memcpy(si, t.sig_info[sig], 104);
        auto regs = j.GetRegisters();
        for (int k = 0; k < 29; k++) mc->x[k] = regs[k];
        mc->fp = regs[29]; mc->lr = regs[30]; mc->sp = j.GetSP(); mc->pc = j.GetPC(); mc->cpsr = j.GetPstate();
        auto vec = j.GetVectors();
        for (int k = 0; k < 32; k++) mc->v[k] = vec[k];
        mc->fpsr = j.GetFpsr(); mc->fpcr = j.GetFpcr();
        uint32_t old_mask = 0, block = g_act[sig].mask.load() | ((flags & kSA_NODEFER) ? 0 : 1u << (sig - 1));
        if (dsigmask) dsigmask(kSIG_BLOCK, &block, &old_mask);
        uc->uc_sigmask = old_mask; uc->uc_mcsize = sizeof(ArmMcontext); uc->uc_mcontext = (u64)mc;
        // the handler runs as a nested call below the frame
        u64 saved_sp = j.GetSP();
        j.SetSP(sp);
        u64 args[3] = {(u64)sig, (u64)si, (u64)uc};
        t.call(h, args, (flags & kSA_SIGINFO) ? 3 : 1);
        j.SetSP(saved_sp);
        if (dsigmask) dsigmask(kSIG_SETMASK, &uc->uc_sigmask, nullptr);
        // resume from the ucontext (handlers may change it)
        for (int k = 0; k < 29; k++) regs[k] = mc->x[k];
        regs[29] = mc->fp; regs[30] = mc->lr;
        j.SetRegisters(regs); j.SetSP(mc->sp); j.SetPC(mc->pc); j.SetPstate(mc->cpsr);
        for (int k = 0; k < 32; k++) vec[k] = mc->v[k];
        j.SetVectors(vec); j.SetFpsr(mc->fpsr); j.SetFpcr(mc->fpcr);
    }
}


// ---- Darling's thread suspension at safe points ---------------------------------------------------------------------
// Darling implements thread_suspend / task_suspend by sending SIGRTMIN (SIGNAL_SIGEXC_SUSPEND) to the Linux thread; its
// handler parks the thread in darlingserver until thread_resume. Delivered wherever the thread is, that stopped
// threads inside the translator, dispatcher or a libarm64jit callback holding the shared emitter mutex or a glibc malloc
// lock: JavaScriptCore's GC (thread_suspend + thread_get_state + conservative scan + thread_resume) then blocked on the
// same lock and never resumed Safari's main thread (Safari black screen). XNU stops a thread in user code, which here
// is guest code or Darling's native libraries, never our runtime. So the signal is wrapped:
//  - interrupted in Darling's native code (a thunk's native callee, or a thread outside guest code), i.e. outside
//    every ELF object (libarm64jit+dynarmic, glibc, libstdc++) and outside guest code: Darling's handler parks it now
//    (a thread blocked in mach_msg, the usual GC target, must be suspendable);
//  - anywhere else (guest code, the JIT, an ELF callback): pending + halt the Jit; it parks at the next safe point:
//    Run() returning to call() (a block boundary), a thunk's entry or its return to guest code. A 1 ms timer re-sends
//    the signal so a thread that went from ELF code into a blocking native call is parked there.
// At every park the guest registers are in the Jit (block boundary or svc), so thread_get_state (ARM flavors) for a
// guest thread is answered from them (Darling's server only has the x86 state).
namespace {
constexpr int kSuspSig = 34;  // LINUX_SIGRTMIN as Darling computes it (glibc SIGRTMIN)
void (*g_darling_susp)(int, siginfo_t *, void *) = nullptr;
std::mutex g_susp_install;
std::atomic<u64> g_n_now{0}, g_n_defer{0}, g_n_safe{0}, g_n_state{0};
struct Range { u64 lo, hi; };
Range g_elf[128];
std::atomic<int> g_nelf{0};
int elf_cb(dl_phdr_info *info, size_t, void *) {
    for (int k = 0; k < info->dlpi_phnum; k++) {
        const ElfW(Phdr) &ph = info->dlpi_phdr[k];
        if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X)) continue;
        int n = g_nelf.load();
        if (n < 128) { g_elf[n] = {info->dlpi_addr + ph.p_vaddr, info->dlpi_addr + ph.p_vaddr + ph.p_memsz}; g_nelf = n + 1; }
    }
    return 0;
}
bool in_elf(u64 rip) {
    for (int k = 0, n = g_nelf.load(std::memory_order_acquire); k < n; k++) if (rip >= g_elf[k].lo && rip < g_elf[k].hi) return true;
    return false;
}
// guest threads by mach port (the suspender names its target by port)
constexpr int kReg = 8192;
std::atomic<GuestThread *> g_reg_t[kReg];
std::atomic<uint32_t> g_reg_port[kReg];
uint32_t port_of(GuestThread &t) { return t.native_tsd ? (uint32_t)t.native_tsd[3] : 0; }  // _PTHREAD_TSD_SLOT_MACH_THREAD_SELF
void reg_port(GuestThread &t) {
    const uint32_t p = port_of(t);
    if (!p) return;
    for (int k = 0; k < kReg; k++) if (g_reg_t[k].load(std::memory_order_relaxed) == &t) { g_reg_port[k].store(p); return; }
}
GuestThread *find_port(uint32_t p) {
    if (!p) return nullptr;
    for (int k = 0; k < kReg; k++) if (g_reg_port[k].load(std::memory_order_acquire) == p) {
        GuestThread *t = g_reg_t[k].load();
        if (t && port_of(*t) == p) return t;
    }
    return nullptr;
}
void arm_timer(GuestThread &t, long ns) {
    if (!t.susp_timer) return;
    itimerspec its{}; its.it_value.tv_nsec = ns;
    timer_settime((timer_t)t.susp_timer, 0, &its, nullptr);
}
void forward(GuestThread *t, int sig, siginfo_t *si, void *uc) {
    if (t) {
        if (t->susp_at_safe.load()) g_n_safe++; else g_n_now++;
        t->susp_pending = 0; t->susp_tries = 0;
        arm_timer(*t, 0);
        reg_port(*t);
        t->susp_parked = 1;
    }
    g_darling_susp(sig, si, uc);
    if (t) t->susp_parked = 0;
}
void susp_handler(int sig, siginfo_t *si, void *ucv) {
    const int saved_errno = errno;
    GuestThread *t = GuestThread::current();
    auto *uc = (ucontext_t *)ucv;
    if (!t || !t->jit || t->susp_at_safe.load()) { forward(t, sig, si, ucv); errno = saved_errno; return; }
    const bool timer = si && si->si_code == SI_TIMER;
    if (timer && !t->susp_pending.load()) { errno = saved_errno; return; }  // stale re-send
    const bool in_guest = t->depth > 0 && !t->native;
    const u64 rip = (u64)uc->uc_mcontext.gregs[REG_RIP];
    // 2 s without a safe point (a GuestThread idle in ELF code for good): park where it is
    if ((!in_guest && !in_elf(rip)) || t->susp_tries > 2000) { forward(t, sig, si, ucv); errno = saved_errno; return; }
    if (!t->susp_pending.load()) g_n_defer++;
    t->susp_tries++;
    t->susp_pending = 1;
    if (in_guest) t->jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
    arm_timer(*t, 1000000);
    errno = saved_errno;
}
void install_wrapper() {
    std::lock_guard<std::mutex> g(g_susp_install);
    struct sigaction cur{};
    if (sigaction(kSuspSig, nullptr, &cur) || !(cur.sa_flags & SA_SIGINFO)) return;
    if (cur.sa_sigaction == susp_handler || (void *)cur.sa_handler == SIG_DFL || (void *)cur.sa_handler == SIG_IGN) return;
    if (!g_nelf.load()) dl_iterate_phdr(elf_cb, nullptr);
    g_darling_susp = cur.sa_sigaction;
    struct sigaction sa = cur;
    sa.sa_sigaction = susp_handler;
    sa.sa_flags = (cur.sa_flags | SA_SIGINFO) & ~SA_RESETHAND;
    sigaction(kSuspSig, &sa, nullptr);
}
bool susp_off() { static const bool v = getenv("X86CODE_A64_SUSP_DEFER") && atoi(getenv("X86CODE_A64_SUSP_DEFER")) == 0; return v; }
}  // namespace

void GuestThread::susp_safe_point() {
    if (!susp_pending.load(std::memory_order_relaxed)) return;
    susp_at_safe = 1;
    syscall(SYS_tgkill, getpid(), tid, kSuspSig);  // delivered before tgkill returns (unless blocked: then later)
    susp_at_safe = 0;
    const u64 n = g_n_safe.load();
    if (n && !(n & (n - 1)))
        logf("suspend: %llu parked at a safe point (deferred %llu), %llu in native code; thread_get_state served %llu",
             (unsigned long long)n, (unsigned long long)g_n_defer.load(), (unsigned long long)g_n_now.load(), (unsigned long long)g_n_state.load());
}

void susp_thread_init(GuestThread &t) {
    t.tid = (int)syscall(SYS_gettid);
    if (susp_off()) return;
    install_wrapper();  // again per thread: Darling may have re-installed its handler (exec, fork)
    sigevent sev{};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = kSuspSig;
    sev._sigev_un._tid = t.tid;
    timer_t tm;
    if (timer_create(CLOCK_MONOTONIC, &sev, &tm) == 0) t.susp_timer = (void *)tm;
    for (int k = 0; k < kReg; k++) {
        GuestThread *e = nullptr;
        if (g_reg_t[k].compare_exchange_strong(e, &t)) { g_reg_port[k].store(port_of(t)); break; }
    }
}
void susp_thread_fini(GuestThread &t) {
    for (int k = 0; k < kReg; k++) if (g_reg_t[k].load() == &t) { g_reg_port[k].store(0); g_reg_t[k].store(nullptr); break; }
    if (t.susp_timer) { timer_delete((timer_t)t.susp_timer); t.susp_timer = nullptr; }
}

// thread_get_state / thread_set_state(thread, flavor, state, count): ARM flavors of a guest thread of this process
// come from / go to its Jit (it is parked at a safe point); anything else goes to Darling.
namespace {
constexpr int kArmThreadState = 1, kArmThreadState64 = 6, kArmNeonState64 = 17;
constexpr uint32_t kTs64Count = 68, kUnifiedCount = 70, kNeonCount = 132;
GuestThread *state_target(GuestThread &t, int flavor) {
    if (flavor != kArmThreadState && flavor != kArmThreadState64 && flavor != kArmNeonState64) return nullptr;
    const uint32_t port = (uint32_t)t.x(0);
    if (port_of(t) == port) return &t;
    GuestThread *g = find_port(port);
    if (!g || !g->jit) return nullptr;
    for (int k = 0; k < 4000 && !g->susp_parked.load(); k++) usleep(50);  // thread_suspend returns before it parks (<= 200 ms)
    return g;
}
}  // namespace
void h_thread_get_state(GuestThread &t, u32 i) {
    const int flavor = (int)t.x(1);
    GuestThread *g = state_target(t, flavor);
    static std::atomic<int> nlog{0};
    if (nlog++ < 4) logf("thread_get_state(%#x, flavor %d): %s", (unsigned)t.x(0), flavor, g ? (g->susp_parked.load() ? "guest thread, parked" : "guest thread, NOT parked") : "to Darling");
    if (!g) { h_generic(t, i); return; }
    auto *cnt = (uint32_t *)t.x(3);
    auto *out = (uint32_t *)t.x(2);
    auto &j = *g->jit;
    if (flavor == kArmNeonState64) {
        if (*cnt < kNeonCount) { t.set_x(0, 4); return; }  // KERN_INVALID_ARGUMENT
        auto v = j.GetVectors();
        memcpy(out, v.data(), 512);
        out[128] = j.GetFpsr(); out[129] = j.GetFpcr(); out[130] = out[131] = 0;
        *cnt = kNeonCount; t.set_x(0, 0); return;
    }
    if (flavor == kArmThreadState) {
        if (*cnt < kUnifiedCount) { t.set_x(0, 4); return; }
        out[0] = kArmThreadState64; out[1] = kTs64Count; out += 2;
        *cnt = kUnifiedCount;
    } else {
        if (*cnt < kTs64Count) { t.set_x(0, 4); return; }
        *cnt = kTs64Count;
    }
    g_n_state++;
    auto *o = (u64 *)out;
    auto r = j.GetRegisters();
    for (int k = 0; k < 31; k++) o[k] = r[k];  // x0-x28, fp, lr
    o[31] = j.GetSP(); o[32] = j.GetPC();
    out[66] = j.GetPstate(); out[67] = 0;
    t.set_x(0, 0);
}
// thread_get_register_pointer_values(thread, uintptr_t *sp, size_t *length, uintptr_t *values): what JavaScriptCore's
// conservative GC really calls (WTF::Thread::getRegisters). Darling's would answer with the host (x86) stack pointer and
// registers of a guest thread: JSC then scanned the native stack range instead of the guest stack. As libsyscall's
// arm64 version: sp minus the 128-byte red zone, lr and x0-x28 + fp values >= the page size.
void h_thread_get_register_pointer_values(GuestThread &t, u32 i) {
    GuestThread *g = state_target(t, kArmThreadState64);
    static std::atomic<int> nlog{0};
    if (nlog++ < 2) logf("thread_get_register_pointer_values(%#x): %s", (unsigned)t.x(0), g ? (g->susp_parked.load() ? "guest thread, parked" : "guest thread, NOT parked") : "to Darling");
    if (!g) { h_generic(t, i); return; }
    auto *sp = (u64 *)t.x(1); auto *len = (size_t *)t.x(2); auto *vals = (u64 *)t.x(3);
    if (!len || (*len > 0 && !vals)) { t.set_x(0, 4); return; }  // KERN_INVALID_ARGUMENT
    g_n_state++;
    auto &j = *g->jit;
    const size_t in = *len;
    size_t out = 0;
    auto push = [&](u64 v) { if (v >= 0x4000) { if (out < in) vals[out] = v; out++; } };
    auto r = j.GetRegisters();
    push(r[30]);
    for (int k = 0; k < 30; k++) push(r[k]);
    if (sp) { const u64 s = j.GetSP(); *sp = s > 128 ? s - 128 : 0; }
    *len = out;
    t.set_x(0, in == 0 || out > in ? 52 /* KERN_INSUFFICIENT_BUFFER_SIZE */ : 0);
}
void h_thread_set_state(GuestThread &t, u32 i) {
    const int flavor = (int)t.x(1);
    GuestThread *g = flavor == kArmNeonState64 ? nullptr : state_target(t, flavor);
    if (!g || g == &t) { h_generic(t, i); return; }
    const uint32_t cnt = (uint32_t)t.x(3);
    auto *in = (const uint32_t *)t.x(2);
    if (flavor == kArmThreadState) { if (cnt < kUnifiedCount) { t.set_x(0, 4); return; } in += 2; }
    else if (cnt < kTs64Count) { t.set_x(0, 4); return; }
    auto *s = (const u64 *)in;
    auto &j = *g->jit;
    std::array<u64, 31> r;
    for (int k = 0; k < 31; k++) r[k] = s[k];
    j.SetRegisters(r); j.SetSP(s[31]); j.SetPC(s[32]); j.SetPstate(in[66]);
    t.set_x(0, 0);
}

}  // namespace a64
