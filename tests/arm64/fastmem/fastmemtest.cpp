// x86Code RAM 3: guest loads/stores/exclusives/atomics through fastmem, with the inline address check (checked) and
// without it (unchecked: a tagged address faults into the fastmem handler). Both must give the same registers and
// memory, for untagged and top-byte-tagged pointers. Guest code: guest.s. Build/run: tests/arm64/fastmem/run.sh
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <array>
#include <sys/mman.h>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

using u8 = std::uint8_t; using u16 = std::uint16_t; using u32 = std::uint32_t; using u64 = std::uint64_t;
using namespace Dynarmic;

static const u32 kGuest[] = {
0x39400002u,0x79400403u,0xb9400404u,0xf9400405u,0xa9411c06u,0x3dc00800u,0x3980c008u,0xb9803409u,0xc8dffc0au,0x9101000bu,
0x39000162u,0x79000563u,0xb9000564u,0xf9000565u,0xa9011d66u,0x3d800960u,0x9102000cu,0xc89ffd85u,0xc85f7d8du,0x910005adu,
0xc80e7d8du,0xf940018fu,0x9100403cu,0xd28000f0u,0xf8300391u,0xf9400392u,0xaa1203f3u,0xd2800c74u,0xc8b37f94u,0xf9400395u,
0x91004018u,0xc87fdf16u,0x91004179u,0xc838db37u,0xa9416979u,0xf940003bu,0xf9000420u,0xd4000001u};

// a loop of tagged loads/stores: past A64EmitX64::kCheckAfterFaults faults its block is retranslated with the check
static const u32 kLoop[] = {0xd2800283u,0xd2800004u,0xf9400002u,0x8b020084u,0xf9000404u,0xf1000463u,0x54ffff81u,0xd4000001u};
// LSE atomics and CAS/CASP on x0 (maybe tagged): atomics.s
static const u32 kAtomics[] = {0x91030009u,0xd28000f0u,0xf8300131u,0xf8308132u,0xf8304133u,0xf8f01134u,0xaa1f03f5u,0xc8b57d30u,
0x91038009u,0xf9400122u,0xf9400523u,0xd28000a4u,0xd28000c5u,0x48227d24u,0xd4000001u};
static const u32* g_code = kGuest;
static size_t g_code_n = std::size(kGuest);

constexpr u64 kTbi = 0x00ffffffffffffffull;
struct Env final : A64::UserCallbacks {
    A64::Jit* jit = nullptr;
    long calls = 0;
    template<typename T> T rd(u64 a) { calls++; T v; std::memcpy(&v, (void*)(a & kTbi), sizeof v); return v; }
    template<typename T> void wr(u64 a, T v) { calls++; std::memcpy((void*)(a & kTbi), &v, sizeof v); }
    std::optional<u32> MemoryReadCode(u64 va) override { return va / 4 < g_code_n ? std::optional<u32>(g_code[va / 4]) : std::optional<u32>(0xD4000001); }
    u8 MemoryRead8(u64 a) override { return rd<u8>(a); }
    u16 MemoryRead16(u64 a) override { return rd<u16>(a); }
    u32 MemoryRead32(u64 a) override { return rd<u32>(a); }
    u64 MemoryRead64(u64 a) override { return rd<u64>(a); }
    A64::Vector MemoryRead128(u64 a) override { return rd<A64::Vector>(a); }
    void MemoryWrite8(u64 a, u8 v) override { wr(a, v); }
    void MemoryWrite16(u64 a, u16 v) override { wr(a, v); }
    void MemoryWrite32(u64 a, u32 v) override { wr(a, v); }
    void MemoryWrite64(u64 a, u64 v) override { wr(a, v); }
    void MemoryWrite128(u64 a, A64::Vector v) override { wr(a, v); }
    template<typename T> bool cx(u64 a, T v, T e) { calls++; return __atomic_compare_exchange_n((T*)(a & kTbi), &e, v, false, 5, 5); }
    bool MemoryWriteExclusive8(u64 a, u8 v, u8 e) override { return cx(a, v, e); }
    bool MemoryWriteExclusive16(u64 a, u16 v, u16 e) override { return cx(a, v, e); }
    bool MemoryWriteExclusive32(u64 a, u32 v, u32 e) override { return cx(a, v, e); }
    bool MemoryWriteExclusive64(u64 a, u64 v, u64 e) override { return cx(a, v, e); }
    bool MemoryWriteExclusive128(u64 a, A64::Vector v, A64::Vector e) override {
        calls++;
        unsigned __int128 ev = e[0] | (unsigned __int128)e[1] << 64, nv = v[0] | (unsigned __int128)v[1] << 64;
        return __atomic_compare_exchange_n((unsigned __int128*)(a & kTbi), &ev, nv, false, 5, 5);
    }
    void InterpreterFallback(u64 pc, size_t) override { std::printf("interpreter fallback %llx\n", (unsigned long long)pc); std::abort(); }
    void CallSVC(u32) override { jit->HaltExecution(); }
    void ExceptionRaised(u64 pc, A64::Exception e) override { std::printf("exception %llx %d\n", (unsigned long long)pc, (int)e); std::abort(); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1000; }
    u64 GetCNTPCT() override { return 0; }
};

struct Result { std::array<u64, 32> x; A64::Vector q0; std::array<u8, 512> mem; u64 mem2[4]; long calls; };

static Result Run(bool unchecked, u64 tag, u8* buf, u64* buf2, bool shared = false) {
    for (int i = 0; i < 256; i++) buf[i] = u8(i * 7 + 3);
    std::memset(buf + 256, 0, 256);
    buf2[0] = 0x1122334455667788ull; buf2[1] = 0; buf2[2] = 5; buf2[3] = 0;
    Env env;
    A64::UserConfig cfg;
    cfg.callbacks = &env;
    static ExclusiveMonitor monitor{1};
    cfg.global_monitor = &monitor;
    cfg.fastmem_pointer = 0;
    cfg.fastmem_address_space_bits = 47;
    cfg.silently_mirror_fastmem = false;
    cfg.fastmem_exclusive_access = true;
    cfg.recompile_on_fastmem_failure = false;
    cfg.recompile_on_exclusive_fastmem_failure = false;
    cfg.unsafe_optimizations = true;
    cfg.optimizations |= OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    cfg.fastmem_unchecked = unchecked;
    std::shared_ptr<A64::Jit::SharedCode> sc;
    if (shared) {
        cfg.shared_translations = true;
        sc = A64::Jit::MakeSharedCode(cfg);
    }
    A64::Jit jit = shared ? A64::Jit{cfg, sc} : A64::Jit{cfg};
    env.jit = &jit;
    for (int i = 0; i < 31; i++) jit.SetRegister(i, 0xdead0000 + i);
    jit.SetRegister(0, u64(buf) | tag);
    jit.SetRegister(1, u64(buf2));
    jit.SetPC(0);
    jit.Run();
    Result r{};
    for (int i = 0; i < 31; i++) r.x[i] = jit.GetRegister(i);
    r.x[0] &= kTbi; r.x[11] &= kTbi; r.x[12] &= kTbi; r.x[24] &= kTbi; r.x[25] &= kTbi; r.x[28] &= kTbi;  // address registers: tag aside
    r.q0 = jit.GetVector(0);
    std::memcpy(r.mem.data(), buf, 512);
    r.mem2[0] = buf2[0]; r.mem2[1] = buf2[1] & kTbi; r.mem2[2] = buf2[2]; r.mem2[3] = buf2[3];
    r.calls = env.calls;
    if (shared) std::printf("  shared: %s\n", A64::Jit::GetMemoryReport(*sc).c_str());
    return r;
}

int main() {
    u8* buf = (u8*)mmap(nullptr, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    u64* buf2 = (u64*)(buf + 4096);
    int fails = 0;
    for (u64 tag : {0ull, 0x5aull << 56, 0x80ull << 56, 0xffull << 56, 0x01ull << 56}) {
        const Result a = Run(false, tag, buf, buf2);
        const Result b = Run(true, tag, buf, buf2);
        bool same = a.x == b.x && a.q0 == b.q0 && a.mem == b.mem && a.mem2[0] == b.mem2[0] && a.mem2[1] == b.mem2[1] && a.mem2[2] == b.mem2[2];
        // untagged: no callback either way; tagged: callbacks in both (checked: inline check, unchecked: faults)
        const bool calls_ok = tag ? (a.calls > 0 && b.calls == a.calls) : (a.calls == 0 && b.calls == 0);
        std::printf("tag %016llx: %s, callbacks checked %ld unchecked %ld%s; x14 %llx x21 %llx x24 %llx\n", (unsigned long long)tag,
                    same ? "same" : "DIFFERENT", a.calls, b.calls, calls_ok ? "" : " (UNEXPECTED)",
                    (unsigned long long)b.x[14], (unsigned long long)b.x[21], (unsigned long long)b.x[24]);
        if (!same) {
            for (int i = 0; i < 31; i++) if (a.x[i] != b.x[i]) std::printf("  x%d %llx vs %llx\n", i, (unsigned long long)a.x[i], (unsigned long long)b.x[i]);
            for (int i = 0; i < 512; i++) if (a.mem[i] != b.mem[i]) { std::printf("  mem[%d] %02x vs %02x\n", i, a.mem[i], b.mem[i]); break; }
        }
        fails += !same || !calls_ok;
    }
    g_code = kLoop; g_code_n = std::size(kLoop);
    for (u64 tag : {0ull, 0x5aull << 56}) {
        const Result a = Run(false, tag, buf, buf2);
        const Result b = Run(true, tag, buf, buf2, true);
        const bool same = a.x[4] == b.x[4] && a.mem == b.mem && a.calls == b.calls;
        std::printf("loop tag %016llx: %s, sum %llx, callbacks %ld / %ld\n", (unsigned long long)tag, same ? "same" : "DIFFERENT",
                    (unsigned long long)b.x[4], a.calls, b.calls);
        fails += !same;
    }
    // atomics: no callback, no fault, same result for every tag in both modes (as the untagged run)
    g_code = kAtomics; g_code_n = std::size(kAtomics);
    Result ref{};
    for (u64 tag : {0ull, 0x5aull << 56, 0xffull << 56, 0x01ull << 56}) {
        for (bool unchecked : {false, true}) {
            Result r = Run(unchecked, tag, buf, buf2);
            r.x[0] = 0; r.x[9] &= kTbi;
            if (!tag && !unchecked) ref = r;
            const bool same = r.x == ref.x && r.mem == ref.mem;
            std::printf("atomics tag %016llx %s: %s, ldadd old %llx cas %llx casp %llx:%llx\n", (unsigned long long)tag,
                        unchecked ? "unchecked" : "checked", same ? "same" : "DIFFERENT", (unsigned long long)r.x[17],
                        (unsigned long long)r.x[21], (unsigned long long)r.x[2], (unsigned long long)r.x[3]);
            if (!same) for (int i = 0; i < 31; i++) if (r.x[i] != ref.x[i]) std::printf("  x%d %llx vs %llx\n", i, (unsigned long long)r.x[i], (unsigned long long)ref.x[i]);
            fails += !same;
        }
    }
    std::printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
