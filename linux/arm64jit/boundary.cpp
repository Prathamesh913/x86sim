// libarm64jit: the svc boundary between guest arm64 code and native x86 Darwin code.
//
// Guest -> native: thunk i is reached by "svc #i" (the runtime itself never executes svc). dynarmic's CallSVC runs the
// handler with the guest state flushed; the handler reads x0-x7/d0-d7 and the guest stack, calls the native function
// (System V: x0-x5 -> rdi..r9, x6/x7 and further stack slots -> the native stack, d0-d7 -> xmm0-7), and writes
// x0/x1/d0/d1. The guest continues at the "ret" after the svc.
// Native -> guest: guest_callable(pc) returns a native trampoline that enters the guest function on the calling thread.
#include "a64.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <mutex>
#include <set>
#include <unordered_map>
#include <sys/mman.h>

namespace a64 {

std::vector<Thunk> g_thunks;
u64 g_exit_stub;
static uint32_t *g_stub_page;  // svc #i; ret for every thunk i (helpers that are not dylib exports use these)
static constexpr size_t kMaxThunks = 0xff00;
// Published length of g_thunks. add_thunk appends under g_thunks_mu then release-stores this; CallSVC
// acquire-loads before indexing. Capacity is reserved to kMaxThunks so data() never moves — without that,
// a concurrent push_back reallocate raced CallSVC's `call *fn` (SIGSEGV at GuestThread::CallSVC+0x7ae,
// same rip offset in every PosterBoard/SpringBoard/WidgetRenderer crash under the shared cache).
static std::mutex g_thunks_mu;
static std::atomic<uint32_t> g_n_thunks{0};

void emit_stub(uint32_t *at, u32 index) {
    at[0] = 0xD4000001u | (index << 5);  // svc #index
    at[1] = 0xD65F03C0u;                 // ret
}

void boundary_init() {
    g_thunks.reserve(kMaxThunks);
    g_stub_page = (uint32_t *)mmap(nullptr, (kMaxThunks + 1) * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_stub_page == MAP_FAILED) die("cannot map the stub page");
    g_exit_stub = (u64)&g_stub_page[kMaxThunks * 2];
    emit_stub(&g_stub_page[kMaxThunks * 2], kExitSvc);
}

u32 add_thunk(const std::string &name, Handler fn, const void *native, int nfixed) {
    std::lock_guard<std::mutex> lock(g_thunks_mu);
    u32 i = (u32)g_thunks.size();
    if (i >= kMaxThunks) die("too many thunks");
    g_thunks.push_back({name, fn, native, nfixed});
    emit_stub(&g_stub_page[2 * i], i);
    g_n_thunks.store(i + 1, std::memory_order_release);
    return i;
}
u64 stub_code(u32 index) { return (u64)&g_stub_page[2 * index]; }
uint32_t thunk_count() { return g_n_thunks.load(std::memory_order_acquire); }

// ---- generic handlers ---------------------------------------------------------------------------------------------
static void ncall(GuestThread &t, const void *fn, u64 *ints, NRet &r) {
    u64 fps[8];
    for (int i = 0; i < 8; i++) fps[i] = t.d(i);
    a64_ncall(fn, ints, fps, &r);
}
static void set_result(GuestThread &t, const NRet &r) {
    t.set_x(0, r.rax); t.set_x(1, r.rdx); t.set_d(0, r.xmm0); t.set_d(1, r.xmm1);
}

void h_generic(GuestThread &t, u32 i) {
    u64 ints[16];
    for (int k = 0; k < 8; k++) ints[k] = t.x(k);
    for (int k = 8; k < 16; k++) ints[k] = t.stack_slot(k - 8);
    NRet r; ncall(t, g_thunks[i].native, ints, r); set_result(t, r);
}
void h_generic_bool(GuestThread &t, u32 i) {
    h_generic(t, i);
    t.set_x(0, t.x(0) & 0xff);
}
// Apple arm64 passes variadic arguments on the stack in 8-byte slots; System V takes them in the next registers.
void h_variadic(GuestThread &t, u32 i) {
    int nf = g_thunks[i].nfixed;
    u64 ints[16];
    for (int k = 0; k < 16; k++) ints[k] = k < nf ? t.x(k) : t.stack_slot(k - nf);
    NRet r; ncall(t, g_thunks[i].native, ints, r); set_result(t, r);
}
void h_noop(GuestThread &, u32) {}
void h_zero(GuestThread &t, u32) { t.set_x(0, 0); }
void h_missing(GuestThread &t, u32 i) {
    static std::mutex mu; static std::set<u32> seen;
    { std::lock_guard l(mu); if (seen.insert(i).second) logf("unimplemented: %s (returns -1/ENOSYS)", g_thunks[i].name.c_str()); }
    *g_darwin->error() = ENOSYS;
    t.set_x(0, ~0ull);
}

// ---- native -> guest trampolines ----------------------------------------------------------------------------------
// 23 bytes each: mov r10, imm64; mov r11, imm64; jmp r11. One per guest function, cached, never freed.
static std::mutex g_tramp_mu;
static std::unordered_map<u64, const void *> g_tramp_of;
static std::unordered_map<const void *, u64> g_guest_of;
static uint8_t *g_tramp_cur, *g_tramp_end;

const void *guest_callable(u64 pc) {
    if (!pc) return nullptr;
    std::lock_guard l(g_tramp_mu);
    if (auto it = g_tramp_of.find(pc); it != g_tramp_of.end()) return it->second;
    if (g_tramp_cur + 32 > g_tramp_end) {
        size_t sz = 1 << 20;
        g_tramp_cur = (uint8_t *)mmap(nullptr, sz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_tramp_cur == MAP_FAILED) die("cannot map trampolines");
        g_tramp_end = g_tramp_cur + sz;
    }
    uint8_t *p = g_tramp_cur;
    g_tramp_cur += 32;
    p[0] = 0x49; p[1] = 0xBA; memcpy(p + 2, &pc, 8);                     // mov r10, pc
    u64 e = (u64)&a64_tramp_entry;
    p[10] = 0x49; p[11] = 0xBB; memcpy(p + 12, &e, 8);                   // mov r11, a64_tramp_entry
    p[20] = 0x41; p[21] = 0xFF; p[22] = 0xE3;                            // jmp r11
    g_tramp_of[pc] = p;
    g_guest_of[p] = pc;
    return p;
}
u64 guest_of_callable(const void *native) {
    std::lock_guard l(g_tramp_mu);
    auto it = g_guest_of.find(native);
    return it == g_guest_of.end() ? 0 : it->second;
}

// ---- printf with guest varargs ------------------------------------------------------------------------------------
// Apple arm64 passes variadic arguments on the stack in 8-byte slots (doubles too) and va_list is a plain pointer to
// those slots, so one formatter serves the ...printf and v...printf forms.
std::string guest_format(const char *f, const u64 *slot) {
    std::string out; char spec[64], buf[512];
    while (*f) {
        if (*f != '%') { out += *f++; continue; }
        if (f[1] == '%') { out += '%'; f += 2; continue; }
        size_t n = 0; spec[n++] = *f++;
        int star_w = -1, star_p = -1;
        while (*f && strchr("-+ #0'", *f) && n < 40) spec[n++] = *f++;
        if (*f == '*') { star_w = (int)*slot++; spec[n++] = *f++; }
        while (*f >= '0' && *f <= '9' && n < 40) spec[n++] = *f++;
        if (*f == '.') {
            spec[n++] = *f++;
            if (*f == '*') { star_p = (int)*slot++; spec[n++] = *f++; }
            while (*f >= '0' && *f <= '9' && n < 40) spec[n++] = *f++;
        }
        bool wide = false;
        while (*f && strchr("hlqjztL", *f) && n < 50) { if (*f != 'h') wide = true; spec[n++] = *f++; }
        char c = *f ? *f++ : 0; spec[n++] = c; spec[n] = 0;
        u64 v = *slot++;
        auto emit = [&](auto arg) {
            if (star_w >= 0 && star_p >= 0) snprintf(buf, sizeof buf, spec, star_w, star_p, arg);
            else if (star_w >= 0) snprintf(buf, sizeof buf, spec, star_w, arg);
            else if (star_p >= 0) snprintf(buf, sizeof buf, spec, star_p, arg);
            else snprintf(buf, sizeof buf, spec, arg);
            out += buf;
        };
        switch (c) {
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d; memcpy(&d, &v, 8);
            for (size_t k = 0; k < n; k++) if (spec[k] == 'L') spec[k] = 'l';
            emit(d); break;
        }
        case 's': emit(v ? (const char *)v : "(null)"); break;
        case 'p': emit((void *)v); break;
        case 'c': emit((int)v); break;
        case 'n': break;
        default: if (wide) emit((long long)v); else emit((int)v); break;
        }
    }
    return out;
}

}  // namespace a64
