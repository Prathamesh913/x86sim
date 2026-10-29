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

}  // namespace a64
