// arm64run: ARMv8.1 LSE atomics and LDAPR, which dynarmic's A64 frontend does not decode. Undecodable instructions
// reach UserCallbacks::InterpreterFallback with the guest state flushed; we perform the operation with a host atomic on
// the identity-mapped address (x86 lock-prefixed ops are sequentially consistent, which covers the A/L orderings) and
// advance the PC. Stage 1 should translate these to IR (native lock xadd/cmpxchg) instead of leaving the JIT.
#include "arm64run.h"

namespace a64r {

namespace {
using u64 = uint64_t;
u64 reg(GuestThread &t, int r) { return r == 31 ? 0 : t.x(r); }
void setreg(GuestThread &t, int r, u64 v) { if (r != 31) t.set_x(r, v); }
u64 base(GuestThread &t, int n) { return n == 31 ? t.sp() : t.x(n); }

template <typename T> u64 rmw(int opc, bool swp, u64 addr, u64 src) {
    using S = std::make_signed_t<T>;
    T *p = (T *)addr, v = (T)src;
    if (swp) return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
    switch (opc) {
    case 0: return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
    case 1: return __atomic_fetch_and(p, (T)~v, __ATOMIC_SEQ_CST);  // LDCLR
    case 2: return __atomic_fetch_xor(p, v, __ATOMIC_SEQ_CST);
    case 3: return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST);       // LDSET
    }
    T old = __atomic_load_n(p, __ATOMIC_SEQ_CST), nv;
    do {
        switch (opc) {
        case 4: nv = (S)old > (S)v ? old : v; break;
        case 5: nv = (S)old < (S)v ? old : v; break;
        case 6: nv = old > v ? old : v; break;
        default: nv = old < v ? old : v; break;
        }
    } while (!__atomic_compare_exchange_n(p, &old, nv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    return old;
}
template <typename T> u64 cas(u64 addr, u64 expected, u64 desired) {
    T e = (T)expected;
    __atomic_compare_exchange_n((T *)addr, &e, (T)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return e;  // old value either way
}
template <typename T> u64 load(u64 addr) { return __atomic_load_n((T *)addr, __ATOMIC_ACQUIRE); }
}  // namespace

bool emulate_lse(GuestThread &t, uint64_t pc, uint32_t insn) {
    int size = insn >> 30, rs = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
    if ((insn & 0x3F200C00) == 0x38200000) {  // atomic memory operations (LDADD..LDUMIN, SWP, LDAPR)
        int o3 = (insn >> 15) & 1, opc = (insn >> 12) & 7;
        u64 a = base(t, rn), old;
        if (o3 && opc == 4 && rs == 31) {      // LDAPR{B,H,}
            static u64 (*const ld[])(u64) = {load<uint8_t>, load<uint16_t>, load<uint32_t>, load<u64>};
            setreg(t, rt, ld[size](a));
        } else {
            if (o3 && opc != 0) return false;
            static u64 (*const f[])(int, bool, u64, u64) = {rmw<uint8_t>, rmw<uint16_t>, rmw<uint32_t>, rmw<u64>};
            old = f[size](opc, o3, a, reg(t, rs));
            setreg(t, rt, old);
        }
    } else if ((insn & 0x3FA07C00) == 0x08A07C00) {  // CAS{A,L,AL}{B,H,}
        static u64 (*const f[])(u64, u64, u64) = {cas<uint8_t>, cas<uint16_t>, cas<uint32_t>, cas<u64>};
        setreg(t, rs, f[size](base(t, rn), reg(t, rs), reg(t, rt)));
    } else if ((insn & 0xBFA07C00) == 0x08207C00) {  // CASP{A,L,AL}
        if ((rs | rt) & 1) return false;
        u64 a = base(t, rn);
        if (insn & (1u << 30)) {
            unsigned __int128 e = ((unsigned __int128)reg(t, rs + 1) << 64) | reg(t, rs);
            unsigned __int128 d = ((unsigned __int128)reg(t, rt + 1) << 64) | reg(t, rt);
            __atomic_compare_exchange_n((unsigned __int128 *)a, &e, d, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            setreg(t, rs, (u64)e); setreg(t, rs + 1, (u64)(e >> 64));
        } else {
            u64 e = (reg(t, rs + 1) << 32) | (uint32_t)reg(t, rs), d = (reg(t, rt + 1) << 32) | (uint32_t)reg(t, rt);
            __atomic_compare_exchange_n((u64 *)a, &e, d, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            setreg(t, rs, (uint32_t)e); setreg(t, rs + 1, e >> 32);
        }
    } else return false;
    t.jit->SetPC(pc + 4);
    return true;
}

}  // namespace a64r
