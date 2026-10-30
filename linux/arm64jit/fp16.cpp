// libarm64jit: FEAT_FP16 Advanced SIMD instructions that dynarmic doesn't translate (ARM64-PLAN stage 2).
//
// iOS 27's RenderBox, CoreImage, Vision, ... use half-precision vector arithmetic (about 70k instructions in the
// shared cache: fmla/fmul by element, three-same arithmetic and compares, conversions). dynarmic hands each of them to
// UserCallbacks::InterpreterFallback; this file executes them on the flushed guest state (lanes computed in binary32
// and rounded to binary16 once, which is exact for add/sub/mul/div/sqrt; fused multiply-add rounds twice). FPCR's
// rounding mode is honoured only by the explicit-rounding conversions; FZ16 and DN are not modelled.
#include "a64.h"
#include <cmath>
#include <cstdint>

namespace a64 {

namespace {
using V = Dynarmic::A64::Vector;
inline float h2f(uint16_t h) { _Float16 f; memcpy(&f, &h, 2); return (float)f; }
inline uint16_t f2h(float x) { _Float16 f = (_Float16)x; uint16_t h; memcpy(&h, &f, 2); return h; }
inline uint16_t lane(const V &v, int i) { return (uint16_t)(v[i / 4] >> (16 * (i % 4))); }
inline void set_lane(V &v, int i, uint16_t h) {
    u64 &w = v[i / 4]; int s = 16 * (i % 4);
    w = (w & ~(0xffffull << s)) | ((u64)h << s);
}
inline bool isnan_h(uint16_t h) { return (h & 0x7c00) == 0x7c00 && (h & 0x3ff); }
inline uint16_t qnan(uint16_t a, uint16_t b) { return (isnan_h(a) ? a : b) | 0x200; }

float fmax_(float a, float b) { return a > b ? a : b; }
float fmin_(float a, float b) { return a < b ? a : b; }
uint16_t maxnm(uint16_t a, uint16_t b) { if (isnan_h(a) && !isnan_h(b)) return b; if (isnan_h(b) && !isnan_h(a)) return a; if (isnan_h(a)) return qnan(a, b); return f2h(fmax_(h2f(a), h2f(b))); }
uint16_t minnm(uint16_t a, uint16_t b) { if (isnan_h(a) && !isnan_h(b)) return b; if (isnan_h(b) && !isnan_h(a)) return a; if (isnan_h(a)) return qnan(a, b); return f2h(fmin_(h2f(a), h2f(b))); }
uint16_t fmax_h(uint16_t a, uint16_t b) { if (isnan_h(a) || isnan_h(b)) return qnan(a, b); float x = h2f(a), y = h2f(b); if (x == 0 && y == 0) return (a & b); return f2h(fmax_(x, y)); }
uint16_t fmin_h(uint16_t a, uint16_t b) { if (isnan_h(a) || isnan_h(b)) return qnan(a, b); float x = h2f(a), y = h2f(b); if (x == 0 && y == 0) return (a | b); return f2h(fmin_(x, y)); }

// three-same: (U, a, opcode) -> lane op. Returns false for unallocated.
bool op3(int U, int a, int opc, uint16_t x, uint16_t y, uint16_t d, uint16_t &r) {
    float fx = h2f(x), fy = h2f(y);
    switch ((U << 4) | (a << 3) | opc) {
    case 0b00000: r = maxnm(x, y); return true;                               // FMAXNM
    case 0b00001: r = f2h(std::fma(fx, fy, h2f(d))); return true;             // FMLA
    case 0b00010: r = f2h(fx + fy); return true;                              // FADD
    case 0b00011:                                                              // FMULX: inf * 0 = +-2
        r = ((std::isinf(fx) && fy == 0) || (fx == 0 && std::isinf(fy))) ? f2h(std::signbit(fx) != std::signbit(fy) ? -2.0f : 2.0f)
                                                                          : f2h(fx * fy);
        return true;
    case 0b00100: r = fx == fy ? 0xffff : 0; return true;                     // FCMEQ
    case 0b00110: r = fmax_h(x, y); return true;                              // FMAX
    case 0b00111: r = f2h(2.0f - fx * fy); return true;                       // FRECPS
    case 0b01000: r = minnm(x, y); return true;                               // FMINNM
    case 0b01001: r = f2h(std::fma(-fx, fy, h2f(d))); return true;            // FMLS
    case 0b01010: r = f2h(fx - fy); return true;                              // FSUB
    case 0b01110: r = fmin_h(x, y); return true;                              // FMIN
    case 0b01111: r = f2h((3.0f - fx * fy) / 2.0f); return true;              // FRSQRTS
    case 0b10011: r = f2h(fx * fy); return true;                              // FMUL
    case 0b10100: r = fx >= fy ? 0xffff : 0; return true;                     // FCMGE
    case 0b10101: r = std::fabs(fx) >= std::fabs(fy) ? 0xffff : 0; return true;  // FACGE
    case 0b10111: r = f2h(fx / fy); return true;                              // FDIV
    case 0b11010: r = f2h(std::fabs(fx - fy)); return true;                   // FABD
    case 0b11100: r = fx > fy ? 0xffff : 0; return true;                      // FCMGT
    case 0b11101: r = std::fabs(fx) > std::fabs(fy) ? 0xffff : 0; return true;  // FACGT
    }
    return false;
}
// pairwise ops of three-same (U=1): FMAXNMP 000, FADDP 010, FMAXP 110; a=1: FMINNMP 000, FMINP 110
bool pairwise(int a, int opc, uint16_t x, uint16_t y, uint16_t &r) {
    switch ((a << 3) | opc) {
    case 0b0000: r = maxnm(x, y); return true;
    case 0b0010: r = f2h(h2f(x) + h2f(y)); return true;
    case 0b0110: r = fmax_h(x, y); return true;
    case 0b1000: r = minnm(x, y); return true;
    case 0b1110: r = fmin_h(x, y); return true;
    }
    return false;
}

float round_mode(float x, int mode) {  // 0 nearest-even, 1 +inf, 2 -inf, 3 zero, 4 ties-away
    switch (mode) {
    case 0: return std::nearbyint(x);
    case 1: return std::ceil(x);
    case 2: return std::floor(x);
    case 3: return std::trunc(x);
    default: return std::round(x);
    }
}
uint16_t cvt_int(float x, int mode, bool is_unsigned) {
    if (std::isnan(x)) return 0;
    float r = round_mode(x, mode);
    if (is_unsigned) return r <= 0 ? 0 : r >= 65535.f ? 0xffff : (uint16_t)r;
    return (uint16_t)(int16_t)(r <= -32768.f ? -32768 : r >= 32767.f ? 32767 : (int)r);
}
int fpcr_mode(GuestThread &t) {
    switch ((t.jit->GetFpcr() >> 22) & 3) { case 0: return 0; case 1: return 1; case 2: return 2; default: return 3; }
}
// two-reg misc: (U, a, opcode) -> lane op
bool op2(GuestThread &t, int U, int a, int opc, uint16_t x, uint16_t &r) {
    float f = h2f(x);
    switch ((U << 6) | (a << 5) | opc) {
    case 0b0011000: r = f2h(round_mode(f, 0)); return true;   // FRINTN
    case 0b0011001: r = f2h(round_mode(f, 2)); return true;   // FRINTM
    case 0b0011010: r = cvt_int(f, 0, false); return true;    // FCVTNS
    case 0b0011011: r = cvt_int(f, 2, false); return true;    // FCVTMS
    case 0b0011100: r = cvt_int(f, 4, false); return true;    // FCVTAS
    case 0b0011101: r = f2h((float)(int16_t)x); return true;  // SCVTF
    case 0b0101100: r = f > 0 ? 0xffff : 0; return true;      // FCMGT #0
    case 0b0101101: r = f == 0 ? 0xffff : 0; return true;     // FCMEQ #0
    case 0b0101110: r = f < 0 ? 0xffff : 0; return true;      // FCMLT #0
    case 0b0101111: r = x & 0x7fff; return true;              // FABS
    case 0b0111000: r = f2h(round_mode(f, 1)); return true;   // FRINTP
    case 0b0111001: r = f2h(round_mode(f, 3)); return true;   // FRINTZ
    case 0b0111010: r = cvt_int(f, 1, false); return true;    // FCVTPS
    case 0b0111011: r = cvt_int(f, 3, false); return true;    // FCVTZS
    case 0b0111101: r = f2h(1.0f / f); return true;           // FRECPE (exact reciprocal; ARM's is an estimate)
    case 0b1011000: r = f2h(round_mode(f, 4)); return true;   // FRINTA
    case 0b1011001: r = f2h(round_mode(f, fpcr_mode(t))); return true;  // FRINTX
    case 0b1011010: r = cvt_int(f, 0, true); return true;     // FCVTNU
    case 0b1011011: r = cvt_int(f, 2, true); return true;     // FCVTMU
    case 0b1011100: r = cvt_int(f, 4, true); return true;     // FCVTAU
    case 0b1011101: r = f2h((float)x); return true;           // UCVTF
    case 0b1101100: r = f >= 0 ? 0xffff : 0; return true;     // FCMGE #0
    case 0b1101101: r = f <= 0 ? 0xffff : 0; return true;     // FCMLE #0
    case 0b1101111: r = x ^ 0x8000; return true;              // FNEG
    case 0b1111001: r = f2h(round_mode(f, fpcr_mode(t))); return true;  // FRINTI
    case 0b1111010: r = cvt_int(f, 1, true); return true;     // FCVTPU
    case 0b1111011: r = cvt_int(f, 3, true); return true;     // FCVTZU
    case 0b1111101: r = f2h(1.0f / std::sqrt(f)); return true;  // FRSQRTE (exact; ARM's is an estimate)
    case 0b1111111: r = f2h(std::sqrt(f)); return true;       // FSQRT
    }
    return false;
}
}  // namespace

// Executes the FP16 SIMD instruction insn (at pc) on t's guest state. False if it isn't one we know.
bool fp16_interpret(GuestThread &t, u32 insn) {
    auto &j = *t.jit;
    const int Rd = insn & 31, Rn = (insn >> 5) & 31, Rm = (insn >> 16) & 31;
    const int Q = (insn >> 30) & 1, U = (insn >> 29) & 1;
    // vector three-same FP16: 0 Q U 01110 a 10 Rm 00 opcode 1 Rn Rd; scalar: 01 U 11110 a 10 Rm 00 opcode 1 Rn Rd
    const bool vec3 = (insn & 0x9F60C400) == 0x0E400400, sc3 = (insn & 0xDF60C400) == 0x5E400400;
    if (vec3 || sc3) {
        const int a = (insn >> 23) & 1, opc = (insn >> 11) & 7;
        const int lanes = sc3 ? 1 : Q ? 8 : 4;
        V n = j.GetVector(Rn), m = j.GetVector(Rm), d = j.GetVector(Rd), r{0, 0};
        if (vec3 && U == 1 && (opc == 0 || opc == 2 || opc == 6) && !(a == 1 && opc == 2)) {  // pairwise
            for (int i = 0; i < lanes; i++) {
                const V &src = i < lanes / 2 ? n : m;
                int k = (i % (lanes / 2)) * 2;
                uint16_t v;
                if (!pairwise(a, opc, lane(src, k), lane(src, k + 1), v)) return false;
                set_lane(r, i, v);
            }
        } else {
            for (int i = 0; i < lanes; i++) {
                uint16_t v;
                if (!op3(U, a, opc, lane(n, i), lane(m, i), lane(d, i), v)) return false;
                set_lane(r, i, v);
            }
        }
        j.SetVector(Rd, r);
        return true;
    }
    // vector by element, size 00 (FP16): 0 Q U 01111 00 L M Rm4 opcode H 0 Rn Rd; scalar: 01 U 11111 00 ...
    const bool vel = (insn & 0x9FC00400) == 0x0F000000, sel = (insn & 0xDFC00400) == 0x5F000000;
    if (vel || sel) {
        const int opc = (insn >> 12) & 15, H = (insn >> 11) & 1, L = (insn >> 21) & 1, M = (insn >> 20) & 1;
        const int idx = (H << 2) | (L << 1) | M, rm = (insn >> 16) & 15;
        const int lanes = sel ? 1 : Q ? 8 : 4;
        int kind;  // 0 fmla 1 fmls 2 fmul 3 fmulx
        if (opc == 0b0001 && !U) kind = 0;
        else if (opc == 0b0101 && !U) kind = 1;
        else if (opc == 0b1001) kind = U ? 3 : 2;
        else return false;
        V n = j.GetVector(Rn), d = j.GetVector(Rd), r = sel ? V{0, 0} : d;
        const uint16_t e = lane(j.GetVector(rm), idx);
        for (int i = 0; i < lanes; i++) {
            uint16_t v;
            if (kind == 0) op3(0, 0, 1, lane(n, i), e, lane(d, i), v);
            else if (kind == 1) op3(0, 1, 1, lane(n, i), e, lane(d, i), v);
            else if (kind == 2) op3(1, 0, 3, lane(n, i), e, 0, v);
            else op3(0, 0, 3, lane(n, i), e, 0, v);
            set_lane(r, i, v);
        }
        if (sel) r = {r[0] & 0xffff, 0};
        else if (!Q) r[1] = 0;
        j.SetVector(Rd, r);
        return true;
    }
    // two-reg misc FP16: 0 Q U 01110 a 111100 opcode 10 Rn Rd; scalar: 01 U 11110 a 111100 opcode 10 Rn Rd
    const bool v2 = (insn & 0x9F3E0C00) == 0x0E380800 && ((insn >> 22) & 1), s2 = (insn & 0xDF3E0C00) == 0x5E380800 && ((insn >> 22) & 1);
    if (v2 || s2) {
        const int a = (insn >> 23) & 1, opc = (insn >> 12) & 31;
        const int lanes = s2 ? 1 : Q ? 8 : 4;
        V n = j.GetVector(Rn), r{0, 0};
        for (int i = 0; i < lanes; i++) {
            uint16_t v;
            if (!op2(t, U, a, opc, lane(n, i), v)) return false;
            set_lane(r, i, v);
        }
        j.SetVector(Rd, r);
        return true;
    }
    // scalar pairwise FP16: 01 0 11110 o1 0 11000 opcode 10 Rn Rd (FMAXNMP 01100, FADDP 01101, FMAXP 01111; o1=1:
    // FMINNMP 01100, FMINP 01111)
    if ((insn & 0xFF3E0C00) == 0x5E300800 && !((insn >> 22) & 1)) {
        const int o1 = (insn >> 23) & 1, opc = (insn >> 12) & 31;
        V n = j.GetVector(Rn);
        uint16_t x = lane(n, 0), y = lane(n, 1), v;
        int p = opc == 0b01100 ? 0 : opc == 0b01101 ? 2 : opc == 0b01111 ? 6 : -1;
        if (p < 0 || (o1 && p == 2) || !pairwise(o1, p, x, y, v)) return false;
        j.SetVector(Rd, {v, 0});
        return true;
    }
    // across lanes FP16: 0 Q 0 01110 o1 0 11000 opcode 10 Rn Rd (FMAXNMV 01100, FMAXV 01111; o1=1 FMINNMV, FMINV)
    if ((insn & 0xBF3E0C00) == 0x0E300800 && !((insn >> 22) & 1) && !U) {
        const int o1 = (insn >> 23) & 1, opc = (insn >> 12) & 31;
        int p = opc == 0b01100 ? 0 : opc == 0b01111 ? 6 : -1;
        if (p < 0) return false;
        V n = j.GetVector(Rn);
        int lanes = Q ? 8 : 4;
        uint16_t acc = lane(n, 0);
        for (int i = 1; i < lanes; i++) pairwise(o1, p, acc, lane(n, i), acc);
        j.SetVector(Rd, {acc, 0});
        return true;
    }
    // Scalar integer -> FP16 (SCVTF/UCVTF Hd, Wn/Xn). dynarmic raises UnallocatedEncoding for
    // type=half (`fltsize == 16` early-out in SCVTF_float_int / UCVTF_float_int). Encoding:
    // sf 0 0 0 1 1 1 1 0 | ftype=11 | 1 | rmode=00 | opcode=010/011 | 000000 | Rn | Rd
    // → 0x1EE20000 (SCVTF) / 0x1EE30000 (UCVTF); sf=1 → 0x9EE2/9EE3. Seen as 1ee20184
    // (scvtf h4,w12) from Metal/RenderBox on iOS 27. Mask clears sf and opcode LSB (U).
    if ((insn & 0x7FFEFC00) == 0x1EE20000) {
        const int rmode_opcode = (insn >> 16) & 0x3f;  // bit21|rmode|opcode → 100010 / 100011
        if (rmode_opcode == 0b100010 || rmode_opcode == 0b100011) {
            const int sf = (insn >> 31) & 1;
            const bool is_unsigned = rmode_opcode & 1;
            const u64 ival = j.GetRegister(Rn);
            float f;
            if (is_unsigned) f = sf ? (float)ival : (float)(uint32_t)ival;
            else f = sf ? (float)(int64_t)ival : (float)(int32_t)ival;
            uint16_t h = f2h(f);
            V v = j.GetVector(Rd);
            v[0] = (v[0] & ~0xffffull) | h;
            j.SetVector(Rd, v);
            return true;
        }
    }
    return false;
}

}  // namespace a64
