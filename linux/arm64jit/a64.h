// libarm64jit: runs arm64 iOS-simulator processes inside Darling (stage 1 of docs/notebook/ARM64-PLAN.md).
//
// The process is an x86_64 macOS program (host/arm64host) that Darling's mldr starts instead of an arm64-only Mach-O.
// It initializes normally with Darling's libSystem, then dlopens this ELF library through Darling's elfcalls and calls
// a64_host_main(). Guest and host share one identity-mapped address space: guest pointer == host pointer.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <optional>
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

namespace a64 {
using u64 = uint64_t;
using u32 = uint32_t;

// Darwin (x86_64 Darling) functions handed over by the Mach-O host program.
struct DarwinApi {
    uint32_t version;
    void *(*dlopen)(const char *, int);
    void *(*dlsym)(void *, const char *);
    int (*dladdr)(const void *, void *);  // Dl_info*
    int *(*error)(void);                  // __error
};
extern const DarwinApi *g_darwin;
void *dsym(const char *name);  // native Darwin symbol (no leading underscore), searched in the three host libraries

// Native results of a guest->native call: rax, rdx, xmm0, xmm1.
struct NRet { u64 rax, rdx, xmm0, xmm1; };
// Calls a native SysV function: ints[0..5] in registers, ints[6..15] on the stack, fps[0..7] in xmm0-7, al = 8.
extern "C" void a64_ncall(const void *fn, const u64 *ints, const u64 *fps, NRet *ret);

struct GuestThread;
using Handler = void (*)(GuestThread &, u32 index);

// One per native thread that runs guest code: its own dynarmic Jit (guest state; a Jit is single-threaded) over the
// process-wide translation cache.
struct GuestThread final : Dynarmic::A64::UserCallbacks {
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    u64 stack_base = 0, stack_size = 0;
    u64 tpidr = 0, tpidrro = 0;
    static constexpr int kGuestTsdSlots = 768;
    u64 *tsd = nullptr;  // the guest's TSD array (tpidrro_el0), see GuestThread()
    u64 *native_tsd = nullptr;
    // The reserved slots 1-9 (errno, MIG reply, QoS class, special reply port, semaphore cache, ...) are shared
    // state: both sides read and write them. Copied across at every guest<->native transition.
    void tsd_to_native() { if (native_tsd) memcpy(native_tsd + 1, tsd + 1, 9 * 8); }
    void tsd_from_native() { if (native_tsd) memcpy(tsd + 1, native_tsd + 1, 9 * 8); }
    bool *done = nullptr;   // the innermost call()'s completion flag; the exit stub sets it
    int depth = 0;          // nesting of call() on this thread (native -> guest -> native -> guest ...)
    u64 svc_count = 0;
    int32_t in_thunk = -1;  // thunk running on this thread (crash reports)
    std::atomic<uint32_t> sig_pending{0};  // async signals recorded by the native handler (signals.cpp)
    uint8_t sig_info[32][104];            // their Darwin siginfo

    GuestThread();
    void reset_after_unwind();  // after a longjmp out of call() (pthread_exit)
    void run_guest_tsd_destructors();  // at thread exit (jit.cpp)
    Dynarmic::A64::UserConfig cfg;
    ~GuestThread() override;
    static GuestThread &self();
    static GuestThread *current();  // null if this thread has none  // the calling native thread's GuestThread (created on first use)

    // Runs guest code at pc with integer args x0..x7 and fp args d0..d7 until it returns to the exit stub.
    // Nested calls (from a native callee of guest code) save and restore the full guest state.
    NRet call(u64 pc, const u64 *ints, int nints, const u64 *fps = nullptr, int nfps = 0);
    u64 call(u64 pc, std::initializer_list<u64> args) { std::vector<u64> v(args); return call(pc, v.data(), (int)v.size()).rax; }

    u64 x(int i) { return jit->GetRegister(i); }
    void set_x(int i, u64 v) { jit->SetRegister(i, v); }
    u64 d(int i) { return jit->GetVector(i)[0]; }
    void set_d(int i, u64 bits) { jit->SetVector(i, {bits, 0}); }
    u64 sp() { return jit->GetSP(); }
    u64 stack_slot(int i) { return *(u64 *)(sp() + 8 * i); }

    // Top-byte-ignore: Darwin arm64 code dereferences pointers with tag bits in the top byte (Swift's _StringObject
    // discriminator). Fastmem sends addresses above 47 bits to these slow-path callbacks, which drop the top byte.
    static constexpr u64 kTbi = 0x00ffffffffffffffull;
    template <typename T> static T rd(u64 a) { T v; __builtin_memcpy(&v, (void *)(a & kTbi), sizeof v); return v; }
    template <typename T> static void wr(u64 a, T v) { __builtin_memcpy((void *)(a & kTbi), &v, sizeof v); }
    std::optional<uint32_t> MemoryReadCode(u64 a) override { return rd<uint32_t>(a); }
    uint8_t MemoryRead8(u64 a) override { return rd<uint8_t>(a); }
    uint16_t MemoryRead16(u64 a) override { return rd<uint16_t>(a); }
    uint32_t MemoryRead32(u64 a) override { return rd<uint32_t>(a); }
    u64 MemoryRead64(u64 a) override { return rd<u64>(a); }
    Dynarmic::A64::Vector MemoryRead128(u64 a) override { return rd<Dynarmic::A64::Vector>(a); }
    void MemoryWrite8(u64 a, uint8_t v) override { wr(a, v); }
    void MemoryWrite16(u64 a, uint16_t v) override { wr(a, v); }
    void MemoryWrite32(u64 a, uint32_t v) override { wr(a, v); }
    void MemoryWrite64(u64 a, u64 v) override { wr(a, v); }
    void MemoryWrite128(u64 a, Dynarmic::A64::Vector v) override { wr(a, v); }
    bool MemoryWriteExclusive8(u64 a, uint8_t v, uint8_t e) override { return __atomic_compare_exchange_n((uint8_t *)(a & kTbi), &e, v, false, 5, 5); }
    bool MemoryWriteExclusive16(u64 a, uint16_t v, uint16_t e) override { return __atomic_compare_exchange_n((uint16_t *)(a & kTbi), &e, v, false, 5, 5); }
    bool MemoryWriteExclusive32(u64 a, uint32_t v, uint32_t e) override { return __atomic_compare_exchange_n((uint32_t *)(a & kTbi), &e, v, false, 5, 5); }
    bool MemoryWriteExclusive64(u64 a, u64 v, u64 e) override { return __atomic_compare_exchange_n((u64 *)(a & kTbi), &e, v, false, 5, 5); }
    bool MemoryWriteExclusive128(u64 a, Dynarmic::A64::Vector v, Dynarmic::A64::Vector e) override;
    void InterpreterFallback(u64 pc, size_t n) override;
    void CallSVC(uint32_t swi) override;
    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception e) override;
    void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, u64 value) override;
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return ~0ull; }
    u64 GetCNTPCT() override;
};

// ---- the svc boundary -------------------------------------------------------------------------------------------
// Guest code reaches native code through 8-byte stubs "svc #i; ret". Thunk i has a handler and a native target.
struct Thunk {
    std::string name;
    Handler fn;
    const void *native = nullptr;  // target of the generic handlers
    int nfixed = 0;                // variadic: number of fixed arguments
};
extern std::vector<Thunk> g_thunks;
constexpr u32 kExitSvc = 0xffff;
// "svc #0xfffe": a Linux syscall for our own arm64 shims (host/simfb's futex wake): x8 = x86_64 syscall number,
// x0-x5 arguments, result (or -errno) in x0. The runtime's code never uses svc immediates this high.
constexpr u32 kLinuxSvc = 0xfffe;
extern u64 g_exit_stub;  // "svc #0xffff": LR of every native->guest call

u32 add_thunk(const std::string &name, Handler fn, const void *native = nullptr, int nfixed = 0);
u64 stub_code(u32 index);              // guest address of thunk i's "svc #i; ret" in the shared stub page
void emit_stub(uint32_t *at, u32 index);  // writes "svc #index; ret" at a guest address
void boundary_init();
uint32_t thunk_count();                // acquire-load of published thunk count (see add_thunk)

// Generic handlers: integer/pointer/fp arguments straight through (x0-x7 -> rdi..r9 + stack, d0-d7 -> xmm0-7).
void h_generic(GuestThread &t, u32 i);
void h_generic_bool(GuestThread &t, u32 i);  // bool result: zero-extend al
void h_variadic(GuestThread &t, u32 i);      // fixed args in registers, the rest from guest stack slots
void h_noop(GuestThread &t, u32 i);           // leaves every register as it is (___chkstk_darwin)
void h_zero(GuestThread &t, u32 i);           // returns 0 (__libkernel_init and friends: native side is initialized)
void h_missing(GuestThread &t, u32 i);        // not provided by Darling: logs once, returns -1/ENOSYS

// Native -> guest: a native function pointer that runs guest function pc on the calling thread's Jit.
const void *guest_callable(u64 pc);
u64 guest_of_callable(const void *native);  // inverse (0 if not a trampoline)
extern "C" void a64_tramp_entry();           // callconv.S

// Special handlers for libsystem functions (handlers.cpp). Returns null if the export needs no special handling.
void h_sigaction(GuestThread &t, u32 i);  // signals.cpp
void h_platform_sigaction(GuestThread &t, u32 i);
void deliver_signals(GuestThread &t);
Handler special_handler(const std::string &name, int *nfixed, const char **native_name);
bool special_needs_native(const std::string &name);
bool is_guest_code_export(const std::string &name);  // setjmp family: emitted as arm64 code in the stub dylib
size_t emit_guest_code(const std::string &name, uint32_t *out, u32 slow_thunk);  // arm64 code for those, returns instruction count

// ---- stub dylibs ------------------------------------------------------------------------------------------------
struct StubLib { std::string install_name; int fd = -1; size_t size = 0; };
// Builds the arm64 stub dylib for one host library (exports from the cache placeholder's export trie).
StubLib build_stub_dylib(const std::string &install_name, const std::vector<std::string> &exports);
// addrs (optional): each export's unslid address (the cache is mapped at its fixed address), ~0 for re-exports.
std::vector<std::string> cache_exports(const std::string &cache_path, const std::string &image, std::vector<u64> *addrs = nullptr,
                                       u64 *text_lo = nullptr, u64 *text_hi = nullptr);
// Native malloc (malloc.cpp): replaces libsystem_malloc's exports in the mapped cache with thunks to Darling's malloc.
void native_malloc_prepare(const std::string &cache_path);  // before dyld_sim maps the cache: reads the exports
void native_malloc_install();
// Native / inline objc_msgSend (objc.cpp).
void native_objc_prepare(const std::string &cache_path);
void native_objc_install();
void native_objc_configure_jit(Dynarmic::A64::UserConfig &c);  // wire inline_* into a new Jit's config
void native_objc_stats();  // hits/misses into the process log
// Persistent JIT cache (dyld shared-cache translations). Keyed by cache UUID + build id.
void jitcache_prepare(const std::string &cache_path);  // read UUID; call before first Jit
void jitcache_set_shared(const std::shared_ptr<Dynarmic::A64::Jit::SharedCode> &sp);
void jitcache_try_load(Dynarmic::A64::Jit::SharedCode &shared);
void jitcache_try_save(Dynarmic::A64::Jit::SharedCode &shared);
void jitcache_save_if_shared();  // no-op when SHARED=0 or no live shared code
u64 reserve_aligned(u64 size, u64 mask);  // guest-VM range [12 GB, 63 GB), PROT_NONE
// gheap.cpp
void gheap_set_native(void (*nfree)(void *), size_t (*nsize)(const void *));
void *gh_malloc(size_t); void *gh_calloc(size_t, size_t); void *gh_realloc(void *, size_t); void gh_free(void *);
void *gh_memalign(size_t align, size_t size); size_t gh_size(const void *); size_t gh_good_size(size_t);                               // from ___libkernel_init: patches the mapped cache text
int stub_fd_for_path(const char *path);  // open() redirect: a dup of the stub dylib's fd, or -1

void set_guest_key_destructor(u64 key, u64 guest_dtor);  // pthread keys (handlers.cpp, jit.cpp)

// misc
void guest_backtrace(GuestThread &t);
void register_stub_symbols(const std::string &file, std::vector<std::pair<u64, std::string>> fn);  // stub dylib exports
std::string where(u64 guest_addr);  // file+offset (non-cache) or "cache"  // frame-pointer chain to the log (jit.cpp)
[[noreturn]] void die(const char *fmt, ...);
void logf(const char *fmt, ...);
extern bool g_trace;
std::string guest_format(const char *fmt, const u64 *slot);  // printf with Apple arm64 vararg slots

}  // namespace a64
