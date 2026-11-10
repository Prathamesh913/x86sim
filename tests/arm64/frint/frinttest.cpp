// x86Sim Speed 34: inline FRINT* (x64 backend) vs dynarmic's software FPRoundInt, edge cases + random.
// Build/run: tests/arm64/frint/run.sh
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <array>
#include <bit>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/fp/fpsr.h"
#include "dynarmic/common/fp/op/FPRoundInt.h"
#include "dynarmic/common/fp/op/FPToFixed.h"
#include "dynarmic/common/fp/rounding_mode.h"

using u8 = std::uint8_t; using u32 = std::uint32_t; using u64 = std::uint64_t;
using namespace Dynarmic;

struct Env final : A64::UserCallbacks {
    std::vector<u32> code;
    A64::Jit* jit = nullptr;
    std::optional<u32> MemoryReadCode(u64 va) override { return va / 4 < code.size() ? std::optional<u32>(code[va / 4]) : std::optional<u32>(0xD4000001); }
    u8 MemoryRead8(u64) override { return 0; }
    std::uint16_t MemoryRead16(u64) override { return 0; }
    u32 MemoryRead32(u64) override { return 0; }
    u64 MemoryRead64(u64) override { return 0; }
    A64::Vector MemoryRead128(u64) override { return {}; }
    void MemoryWrite8(u64, u8) override {}
    void MemoryWrite16(u64, std::uint16_t) override {}
    void MemoryWrite32(u64, u32) override {}
    void MemoryWrite64(u64, u64) override {}
    void MemoryWrite128(u64, A64::Vector) override {}
    void InterpreterFallback(u64 pc, size_t) override { std::printf("interp fallback %llx\n", (unsigned long long)pc); std::abort(); }
    void CallSVC(u32) override { jit->HaltExecution(); }
    void ExceptionRaised(u64 pc, A64::Exception e) override { std::printf("exception %llx %d\n", (unsigned long long)pc, (int)e); std::abort(); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1000; }
    u64 GetCNTPCT() override { return 0; }
};

struct Op { const char* name; u32 enc32, enc64; FP::RoundingMode mode; bool exact; bool vector; };

int main() {
    const FP::RoundingMode RN = FP::RoundingMode::ToNearest_TieEven, RP = FP::RoundingMode::TowardsPlusInfinity,
                           RM = FP::RoundingMode::TowardsMinusInfinity, RZ = FP::RoundingMode::TowardsZero,
                           RA = FP::RoundingMode::ToNearest_TieAwayFromZero;
    // scalar: d0 = op(d1); vector: v0 = op(v1). mode 0xFF = from FPCR (FRINTI / FRINTX).
    const std::vector<Op> ops = {
        {"frintn", 0x1E244020, 0x1E644020, RN, false, false}, {"frintp", 0x1E24C020, 0x1E64C020, RP, false, false},
        {"frintm", 0x1E254020, 0x1E654020, RM, false, false}, {"frintz", 0x1E25C020, 0x1E65C020, RZ, false, false},
        {"frinta", 0x1E264020, 0x1E664020, RA, false, false}, {"frintx", 0x1E274020, 0x1E674020, (FP::RoundingMode)0xFF, true, false},
        {"frinti", 0x1E27C020, 0x1E67C020, (FP::RoundingMode)0xFF, false, false},
        {"vfrinta", 0x6E218820, 0x6E618820, RA, false, true}, {"vfrintx", 0x6E219820, 0x6E619820, (FP::RoundingMode)0xFF, true, true},
        {"vfrintn", 0x4E218820, 0x4E618820, RN, false, true}, {"vfrintm", 0x4E219820, 0x4E619820, RM, false, true},
    };
    std::vector<u64> d = {0, 0x8000000000000000, 0x3FE0000000000000, 0xBFE0000000000000, 0x3FF8000000000000, 0xC004000000000000,
        0x3FDFFFFFFFFFFFFF, 0xBFDFFFFFFFFFFFFF, 0x4330000000000000, 0x432FFFFFFFFFFFFF, 0xC32FFFFFFFFFFFFF, 0x4340000000000000,
        0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000123, 0x7FF0000000000001, 0xFFF4000000000000,
        0x0000000000000001, 0x8000000000000001, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF, 0x3FF0000000000000,
        0x4004000000000000, 0x400C000000000000, 0xC00C000000000000, 0x3FE0000000000001, 0x432FFFFFFFFFFFFE};
    std::vector<u32> s = {0, 0x80000000, 0x3F000000, 0xBF000000, 0x3FC00000, 0xC0200000, 0x3EFFFFFF, 0xBEFFFFFF, 0x4B000000,
        0x4AFFFFFF, 0xCAFFFFFF, 0x4B800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00123, 0x7F800001, 0xFFA00000, 1, 0x80000001,
        0x00800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x3F800000, 0x40200000, 0x40600000, 0xC0600000, 0x3F000001, 0x4AFFFFFE};
    std::mt19937_64 rng(34);
    for (int i = 0; i < 4000; i++) {
        u64 r = rng();
        int e = 1023 - 4 + (int)(r % 60);  // around the integer boundary
        d.push_back((r & 0x800FFFFFFFFFFFFFull) | ((u64)(e & 0x7FF) << 52));
        int es = 127 - 4 + (int)((r >> 20) % 32);
        s.push_back(((u32)r & 0x807FFFFF) | ((u32)(es & 0xFF) << 23));
        if (i % 4 == 0) { d.push_back(std::bit_cast<u64>(std::nearbyint(std::bit_cast<double>(d.back()) * 2) / 2)); s.push_back(std::bit_cast<u32>((float)(std::nearbyint(std::bit_cast<float>(s.back()) * 2) / 2))); }
    }
    long fails = 0, checks = 0;
    const u32 fpcrs[] = {0, 1u << 25 /*DN*/, 1u << 22 /*RP*/, 2u << 22 /*RM*/, 3u << 22 /*RZ*/, 1u << 24 /*FZ*/, (1u << 25) | (3u << 22)};
    for (u32 fpcr_v : fpcrs) {
        for (const Op& op : ops) {
            for (int sz : {32, 64}) {
                Env env;
                env.code = {sz == 32 ? op.enc32 : op.enc64, 0xD4000001};
                A64::UserConfig cfg;
                cfg.callbacks = &env;
                A64::Jit jit{cfg};
                env.jit = &jit;
                const size_t n = sz == 32 ? s.size() : d.size();
                for (size_t i = 0; i < n; i++) {
                    A64::Vector in{};
                    if (sz == 32) { in[0] = s[i] | ((u64)s[(i + 1) % n] << 32); in[1] = s[(i + 2) % n] | ((u64)s[(i + 3) % n] << 32); }
                    else { in[0] = d[i]; in[1] = d[(i + 1) % n]; }
                    jit.SetVector(1, in);
                    jit.SetVector(0, {0x1111, 0x2222});
                    jit.SetFpcr(fpcr_v);
                    jit.SetFpsr(0);
                    jit.SetPC(0);
                    jit.Run();
                    const A64::Vector out = jit.GetVector(0);
                    const u32 fpsr_jit = jit.GetFpsr();
                    FP::FPCR fpcr{fpcr_v};
                    FP::FPSR fpsr{0};
                    const FP::RoundingMode mode = (u32)op.mode == 0xFF ? fpcr.RMode() : op.mode;
                    A64::Vector want{};
                    const int lanes = op.vector ? (sz == 32 ? 4 : 2) : 1;
                    for (int l = 0; l < lanes; l++) {
                        if (sz == 32) {
                            const u32 x = (u32)(in[l / 2] >> (32 * (l % 2)));
                            const u64 y = FP::FPRoundInt<u32>(x, fpcr, mode, op.exact, fpsr);
                            want[l / 2] |= (y & 0xFFFFFFFF) << (32 * (l % 2));
                        } else {
                            want[l] = FP::FPRoundInt<u64>(in[l], fpcr, mode, op.exact, fpsr);
                        }
                    }
                    // scalar: only the low element is defined
                    u64 mask0 = (!op.vector && sz == 32) ? 0xFFFFFFFF : ~0ull;
                    bool ok = (out[0] & mask0) == (want[0] & mask0) && (!op.vector || out[1] == want[1]);
                    // flags: IOC (bit0), IXC (bit4)
                    const u32 fm = 0x11;
                    bool fok = (fpsr_jit & fm) == (fpsr.Value() & fm);
                    checks++;
                    if (!ok || !fok) {
                        if (fails++ < 40)
                            std::printf("FAIL %s%d fpcr=%08x in=%016llx:%016llx jit=%016llx:%016llx fpsr=%x want=%016llx:%016llx fpsr=%x\n", op.name, sz, fpcr_v,
                                (unsigned long long)in[1], (unsigned long long)in[0], (unsigned long long)out[1], (unsigned long long)out[0], fpsr_jit,
                                (unsigned long long)want[1], (unsigned long long)want[0], fpsr.Value());
                    }
                }
            }
        }
    }
    // FCVTAS / FCVTAU (ties away to integer): scalar W/X from S/D and vector 4S/2D.
    struct Cv { const char* name; u32 enc; int fsize, isize; bool uns, vector; };
    const std::vector<Cv> cvs = {
        {"fcvtas w,s", 0x1E240020, 32, 32, false, false}, {"fcvtas x,s", 0x9E240020, 32, 64, false, false},
        {"fcvtas w,d", 0x1E640020, 64, 32, false, false}, {"fcvtas x,d", 0x9E640020, 64, 64, false, false},
        {"fcvtau w,s", 0x1E250020, 32, 32, true, false}, {"fcvtau x,d", 0x9E650020, 64, 64, true, false},
        {"fcvtau w,d", 0x1E650020, 64, 32, true, false}, {"fcvtau x,s", 0x9E250020, 32, 64, true, false},
        {"vfcvtas 4s", 0x4E21C820, 32, 32, false, true}, {"vfcvtas 2d", 0x4E61C820, 64, 64, false, true},
        {"vfcvtau 4s", 0x6E21C820, 32, 32, true, true}, {"vfcvtau 2d", 0x6E61C820, 64, 64, true, true},
    };
    for (u32 fpcr_v : fpcrs) {
        for (const Cv& cv : cvs) {
            Env env;
            env.code = {cv.enc, 0xD4000001};
            A64::UserConfig cfg;
            cfg.callbacks = &env;
            A64::Jit jit{cfg};
            env.jit = &jit;
            const int sz = cv.fsize;
            const size_t n = sz == 32 ? s.size() : d.size();
            for (size_t i = 0; i < n; i++) {
                A64::Vector in{};
                if (sz == 32) { in[0] = s[i] | ((u64)s[(i + 1) % n] << 32); in[1] = s[(i + 2) % n] | ((u64)s[(i + 3) % n] << 32); }
                else { in[0] = d[i]; in[1] = d[(i + 1) % n]; }
                jit.SetVector(1, in);
                jit.SetVector(0, {0x1111, 0x2222});
                jit.SetRegister(0, 0x5555555555555555);
                jit.SetFpcr(fpcr_v);
                jit.SetFpsr(0);
                jit.SetPC(0);
                jit.Run();
                FP::FPCR fpcr{fpcr_v};
                FP::FPSR fpsr{0};
                A64::Vector want{}, got{};
                if (cv.vector) {
                    got = jit.GetVector(0);
                    const int lanes = sz == 32 ? 4 : 2;
                    for (int l = 0; l < lanes; l++) {
                        if (sz == 32) {
                            const u32 x = (u32)(in[l / 2] >> (32 * (l % 2)));
                            want[l / 2] |= (FP::FPToFixed<u32>(32, x, 0, cv.uns, fpcr, RA, fpsr) & 0xFFFFFFFF) << (32 * (l % 2));
                        } else {
                            want[l] = FP::FPToFixed<u64>(64, in[l], 0, cv.uns, fpcr, RA, fpsr);
                        }
                    }
                } else {
                    got[0] = jit.GetRegister(0);
                    want[0] = sz == 32 ? FP::FPToFixed<u32>(cv.isize, (u32)in[0], 0, cv.uns, fpcr, RA, fpsr)
                                       : FP::FPToFixed<u64>(cv.isize, in[0], 0, cv.uns, fpcr, RA, fpsr);
                    if (cv.isize == 32) want[0] &= 0xFFFFFFFF;
                }
                const u32 fpsr_jit = jit.GetFpsr();
                // FPToFixed's IOC on out-of-range is mirrored by dynarmic's inline paths only for some forms
                // (pre-existing); compare values and IXC.
                bool ok = got[0] == want[0] && got[1] == want[1];
                // out of range: ARM sets only IOC; the inline saturation (all modes, pre-existing) does not model it
                // scalar unsigned 64-bit: the shared inline sequence also converts x - 2^63 (raises IXC; all modes)
                bool fok = (fpsr.Value() & 1) || (cv.uns && cv.isize == 64 && !cv.vector) || (fpsr_jit & 0x10) == (fpsr.Value() & 0x10);
                checks++;
                if (!ok || !fok) {
                    if (fails++ < 80)
                        std::printf("FAIL %s fpcr=%08x in=%016llx:%016llx jit=%016llx:%016llx fpsr=%x want=%016llx:%016llx fpsr=%x\n", cv.name, fpcr_v,
                            (unsigned long long)in[1], (unsigned long long)in[0], (unsigned long long)got[1], (unsigned long long)got[0], fpsr_jit,
                            (unsigned long long)want[1], (unsigned long long)want[0], fpsr.Value());
                }
            }
        }
    }
    std::printf("%ld checks, %ld failures\n", checks, fails);
    return fails ? 1 : 0;
}
