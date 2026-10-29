// arm64run: shared declarations. Guest and host share one identity-mapped address space (guest pointer == host pointer).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

namespace a64r {

struct GuestThread;
using Handler = void (*)(GuestThread &);

// One per guest thread: its own dynarmic Jit (dynarmic Jits are single-threaded), sharing the global exclusive monitor.
struct GuestThread final : Dynarmic::A64::UserCallbacks {
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    uint64_t stack_base = 0, stack_size = 0;
    bool exited = false;
    uint64_t svc_count = 0, lse_count = 0;

    explicit GuestThread(size_t processor_id);
    ~GuestThread() override;
    uint64_t call(uint64_t pc, std::initializer_list<uint64_t> args);  // run guest code until it returns to the exit stub

    uint64_t x(int i) { return jit->GetRegister(i); }
    void set_x(int i, uint64_t v) { jit->SetRegister(i, v); }
    double d(int i);
    void set_d0(double v);
    uint64_t sp() { return jit->GetSP(); }

    // UserCallbacks: fastmem covers all accesses; these are the slow path (identity mapped, so plain host accesses).
    template <typename T> static T rd(uint64_t a) { T v; __builtin_memcpy(&v, (void *)a, sizeof v); return v; }
    template <typename T> static void wr(uint64_t a, T v) { __builtin_memcpy((void *)a, &v, sizeof v); }
    std::optional<uint32_t> MemoryReadCode(uint64_t a) override { return rd<uint32_t>(a); }
    uint8_t MemoryRead8(uint64_t a) override { return rd<uint8_t>(a); }
    uint16_t MemoryRead16(uint64_t a) override { return rd<uint16_t>(a); }
    uint32_t MemoryRead32(uint64_t a) override { return rd<uint32_t>(a); }
    uint64_t MemoryRead64(uint64_t a) override { return rd<uint64_t>(a); }
    Dynarmic::A64::Vector MemoryRead128(uint64_t a) override { return rd<Dynarmic::A64::Vector>(a); }
    void MemoryWrite8(uint64_t a, uint8_t v) override { wr(a, v); }
    void MemoryWrite16(uint64_t a, uint16_t v) override { wr(a, v); }
    void MemoryWrite32(uint64_t a, uint32_t v) override { wr(a, v); }
    void MemoryWrite64(uint64_t a, uint64_t v) override { wr(a, v); }
    void MemoryWrite128(uint64_t a, Dynarmic::A64::Vector v) override { wr(a, v); }
    bool MemoryWriteExclusive8(uint64_t a, uint8_t v, uint8_t e) override { return __atomic_compare_exchange_n((uint8_t *)a, &e, v, false, 5, 5); }
    bool MemoryWriteExclusive16(uint64_t a, uint16_t v, uint16_t e) override { return __atomic_compare_exchange_n((uint16_t *)a, &e, v, false, 5, 5); }
    bool MemoryWriteExclusive32(uint64_t a, uint32_t v, uint32_t e) override { return __atomic_compare_exchange_n((uint32_t *)a, &e, v, false, 5, 5); }
    bool MemoryWriteExclusive64(uint64_t a, uint64_t v, uint64_t e) override { return __atomic_compare_exchange_n((uint64_t *)a, &e, v, false, 5, 5); }
    bool MemoryWriteExclusive128(uint64_t a, Dynarmic::A64::Vector v, Dynarmic::A64::Vector e) override;
    void InterpreterFallback(uint64_t pc, size_t n) override;
    void CallSVC(uint32_t swi) override;
    void ExceptionRaised(uint64_t pc, Dynarmic::A64::Exception e) override;
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return ~0ull; }
    uint64_t GetCNTPCT() override;
};

extern thread_local GuestThread *current;
extern Dynarmic::ExclusiveMonitor *g_monitor;

// Thunk table: GOT entries of imported functions point at "svc #index; ret" stubs in the thunk page.
struct Thunk { std::string name; Handler fn; };
extern std::vector<Thunk> g_thunks;
extern uint64_t g_exit_stub;          // "svc #0xffff": guest code returning here halts its Jit
constexpr uint32_t kExitSvc = 0xffff;
uint64_t thunk_for(const std::string &sym);        // address to bind a symbol to (function stub or native data)
void thunks_init();

// LSE (ARMv8.1 atomics) and LDAPR emulation, used from InterpreterFallback: true if handled.
bool emulate_lse(GuestThread &t, uint64_t pc, uint32_t insn);

// Mach-O loader.
struct Image { uint64_t base = 0, entry = 0; std::string path; };
Image load_macho(const char *path);

}  // namespace a64r
