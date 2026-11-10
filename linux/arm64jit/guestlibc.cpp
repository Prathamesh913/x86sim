// Speed 6: arm64 versions of the hottest pure libsystem_platform exports, run as guest code under the JIT instead of
// an svc to Darling's x86 libsystem. An svc round trip costs ~40-60 ns (leaves the JIT's run loop, X86CODE_A64_SVCBENCH);
// these take ~10-15 ns for typical short strings. memmove/bzero hand sizes >= 256 bytes to the native function
// (the `svc #0` in their code is patched to that thunk). Preferences calls these ~50M times in a Settings open.
// Sources: tests/arm64/guestlibc/*.s (llvm-mc -triple=aarch64). X86CODE_A64_GUEST_LIBC=0 keeps every one native;
// X86CODE_A64_GUESTLIB_TEST=1 runs a randomized comparison against glibc and exits.
#include "a64.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/mman.h>

namespace a64 {

static const uint32_t kMemmove[] = {
    0xf104005fu, 0x54000422u, 0xaa0003e3u, 0xb40003c2u, 0xeb01007fu, 0x54000229u, 0x8b020024u, 0xeb04007fu,
    0x540001c2u, 0x8b020021u, 0x8b020063u, 0xf100205fu, 0x540000a3u, 0xf85f8c25u, 0xf81f8c65u, 0xd1002042u,
    0x17fffffbu, 0xb4000202u, 0x385ffc25u, 0x381ffc65u, 0xd1000442u, 0x17fffffcu, 0xf100205fu, 0x540000a3u,
    0xf8408425u, 0xf8008465u, 0xd1002042u, 0x17fffffbu, 0xb40000a2u, 0x38401425u, 0x38001465u, 0xd1000442u,
    0x17fffffcu, 0xd65f03c0u, 0xd4000001u, 0xd65f03c0u,
};
static const uint32_t kBzero[] = {
    0xf104003fu, 0x54000182u, 0xaa0003e3u, 0xf100203fu, 0x54000083u, 0xf800847fu, 0xd1002021u, 0x17fffffcu,
    0xb4000081u, 0x3800147fu, 0xd1000421u, 0x17fffffdu, 0xd65f03c0u, 0xd4000001u, 0xd65f03c0u,
};
static const uint32_t kStrcmp[] = {
    0xb200c3e6u, 0xb201c3e7u, 0x92402c04u, 0xf13fe09fu, 0x54000308u, 0x92402c24u, 0xf13fe09fu, 0x540002a8u,
    0xf9400002u, 0xf9400023u, 0xcb060044u, 0x8a220084u, 0x8a070084u, 0xca030045u, 0xaa0400a5u, 0xb5000085u,
    0x91002000u, 0x91002021u, 0x17fffff0u, 0xdac000a5u, 0xdac010a5u, 0x927d08a5u, 0x9ac52442u, 0x9ac52463u,
    0x12001c42u, 0x12001c63u, 0x4b030040u, 0xd65f03c0u, 0x38401402u, 0x38401423u, 0x7100045fu, 0x7a432040u,
    0x54fffc40u, 0x4b030040u, 0xd65f03c0u,
};
static const uint32_t kMemcmp[] = {
    0xf100205fu, 0x54000263u, 0xf9400004u, 0xf9400025u, 0xeb05009fu, 0x540000a1u, 0x91002000u, 0x91002021u,
    0xd1002042u, 0x17fffff7u, 0xca050086u, 0xdac000c6u, 0xdac010c6u, 0x927d08c6u, 0x9ac62484u, 0x9ac624a5u,
    0x12001c84u, 0x12001ca5u, 0x4b050080u, 0xd65f03c0u, 0xb4000102u, 0x38401404u, 0x38401425u, 0xd1000442u,
    0x6b05009fu, 0x54ffff60u, 0x4b050080u, 0xd65f03c0u, 0x52800000u, 0xd65f03c0u,
};
static const uint32_t kStrlen[] = {
    0xaa0003e1u, 0xf240083fu, 0x540000a0u, 0x39400022u, 0x340001c2u, 0x91000421u, 0x17fffffbu, 0xb200c3e4u,
    0xf9400022u, 0xcb040043u, 0x8a220063u, 0xf201c063u, 0x54000061u, 0x91002021u, 0x17fffffau, 0xdac00063u,
    0xdac01063u, 0x8b430c21u, 0xcb000020u, 0xd65f03c0u,
};

// Perf 15: the next hottest platform calls of a cold Settings launch (strncmp ~820K, memchr ~210K, strchr ~210K;
// strnlen) as byte loops (memchr scans 64 bytes, then hands the rest to the native function), and os_unfair_lock's uncontended paths
// (~2M lock/unlock calls): one exclusive pair on the lock word with the owner value libplatform uses (TSD slot 3, the
// thread's mach port, shared by the guest and native TSD); contention, waiters or a foreign owner go native (svc #0).
// Sources: tests/arm64/guestlibc/{strncmp,memchr,strchr,strnlen,unfair_lock,unfair_unlock}.s.
// X86CODE_A64_GUEST_LOCKS=0 keeps the lock functions native.
static const uint32_t kStrncmp[] = {0xb4000102u, 0x38401403u, 0x38401424u, 0x6b040065u, 0x540000c1u, 0x34000063u,
                                    0xf1000442u, 0x54ffff41u, 0x52800000u, 0xd65f03c0u, 0x2a0503e0u, 0xd65f03c0u};
static const uint32_t kMemchr[] = {0x12001c21u, 0xb4000182u, 0xd2800804u, 0x39400003u, 0x6b01007fu, 0x54000120u, 0x91000400u, 0xf1000442u, 0x540000a0u, 0xf1000484u, 0x54ffff21u, 0xd4000001u, 0xd65f03c0u, 0xd2800000u, 0xd65f03c0u};
static const uint32_t kStrchr[] = {0x12001c21u, 0x39400003u, 0x6b01007fu, 0x540000a0u, 0x34000063u,
                                   0x91000400u, 0x17fffffbu, 0xd2800000u, 0xd65f03c0u};
static const uint32_t kStrnlen[] = {0xaa0003e2u, 0xb40000c1u, 0x39400043u, 0x34000083u, 0x91000442u,
                                    0xf1000421u, 0x54ffff81u, 0xcb000040u, 0xd65f03c0u};
static const uint32_t kUnfairLock[] = {0xd53bd068u, 0xb9401909u, 0x885ffc0au, 0x3500008au, 0x880b7c09u,
                                       0x35ffffabu, 0xd65f03c0u, 0xd5033f5fu, 0xd4000001u, 0xd65f03c0u};
static const uint32_t kUnfairUnlock[] = {0xd53bd068u, 0xb9401909u, 0x885f7c0au, 0x6b09015fu, 0x54000081u, 0x880bfc1fu,
                                         0x35ffff8bu, 0xd65f03c0u, 0xd5033f5fu, 0xd4000001u, 0xd65f03c0u};

struct GuestFn { const char *name; const uint32_t *code; size_t n; };
static const GuestFn kFns[] = {
    {"__platform_memmove", kMemmove, sizeof kMemmove / 4}, {"__platform_memcpy", kMemmove, sizeof kMemmove / 4},
    {"__platform_bzero", kBzero, sizeof kBzero / 4},       {"__platform_strcmp", kStrcmp, sizeof kStrcmp / 4},
    {"__platform_memcmp", kMemcmp, sizeof kMemcmp / 4},     {"__platform_strlen", kStrlen, sizeof kStrlen / 4},
    {"__platform_strncmp", kStrncmp, sizeof kStrncmp / 4}, {"__platform_memchr", kMemchr, sizeof kMemchr / 4},
    {"__platform_strchr", kStrchr, sizeof kStrchr / 4},     {"__platform_strnlen", kStrnlen, sizeof kStrnlen / 4},
    {"_os_unfair_lock_lock", kUnfairLock, sizeof kUnfairLock / 4},
    {"_os_unfair_lock_lock_with_options", kUnfairLock, sizeof kUnfairLock / 4},
    {"_os_unfair_lock_unlock", kUnfairUnlock, sizeof kUnfairUnlock / 4},
};

static const GuestFn *find(const std::string &n) {
    static const bool on = !(getenv("X86CODE_A64_GUEST_LIBC") && atoi(getenv("X86CODE_A64_GUEST_LIBC")) == 0);
    if (!on) return nullptr;
    static const bool locks = !(getenv("X86CODE_A64_GUEST_LOCKS") && atoi(getenv("X86CODE_A64_GUEST_LOCKS")) == 0);
    if (!locks && n.rfind("_os_unfair_lock", 0) == 0) return nullptr;
    for (const auto &f : kFns) if (n == f.name) return &f;
    return nullptr;
}

bool is_guest_libc_export(const std::string &n) { return find(n) != nullptr; }

// Copies the function to o (<= 64 words); `svc #0` becomes `svc #slow` (the native function's thunk).
size_t emit_guest_libc(const std::string &n, uint32_t *o, u32 slow) {
    const GuestFn *f = find(n);
    for (size_t i = 0; i < f->n; i++) o[i] = f->code[i] == 0xd4000001u ? 0xd4000001u | slow << 5 : f->code[i];
    return f->n;
}

// X86CODE_A64_SVCBENCH=n: time n calls of strlen through its svc thunk and as guest code, then exit.
// X86CODE_A64_GUESTLIB_TEST=1: compare each guest function with glibc on random inputs, then exit.
void svc_bench(GuestThread &t) {
    const char *e = getenv("X86CODE_A64_SVCBENCH");
    const bool test = getenv("X86CODE_A64_GUESTLIB_TEST") != nullptr;
    if (!e && !test) return;
    // bench(fn, arg, n): call fn(arg) n times
    static const uint32_t kLoop[] = {0xa9be53f3u, 0xa9017bf5u, 0xaa0003f3u, 0xaa0103f4u, 0xaa0203f5u, 0xaa1403e0u,
                                     0xd63f0260u, 0xf10006b5u, 0x54ffffa1u, 0xa9417bf5u, 0xa8c253f3u, 0xd65f03c0u};
    auto *page = (uint32_t *)mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(page, kLoop, sizeof kLoop);
    u64 at[16]{};
    for (size_t k = 0; k < 10; k++) {
        const void *nat = dsym(kFns[k].name + 1);
        at[k] = (u64)(page + 64 * (k + 1));
        emit_guest_libc(kFns[k].name, page + 64 * (k + 1), nat ? add_thunk(kFns[k].name, h_generic, nat) : 0);
    }
    if (e) {
        const u64 n = strtoull(e, nullptr, 0);
        static const char str[] = "com.apple.springboard.preferences";  // 33 bytes
        u64 svc = 0;
        for (u32 i = 0, m = thunk_count(); i < m && !svc; i++) if (g_thunks[i].name == "__platform_strlen") svc = stub_code(i);
        for (u64 fn : {svc, at[5], svc, at[5]}) {
            timespec a, b;
            clock_gettime(CLOCK_MONOTONIC, &a);
            u64 args[3] = {fn, (u64)str, n};
            t.call((u64)page, args, 3);
            clock_gettime(CLOCK_MONOTONIC, &b);
            fprintf(stderr, "svcbench: %s strlen %.1f ns/call\n", fn == svc ? "svc" : "guest",
                    ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / (double)n);
        }
    }
    if (test) {
        srandom(1);
        auto *buf = (unsigned char *)mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        auto *ref = (unsigned char *)malloc(0x10000);
        int bad = 0, runs = 0;
        auto sgn = [](long v) { return v < 0 ? -1 : v > 0; };
        for (int it = 0; it < 20000; it++) {
            for (int i = 0; i < 0x10000; i++) buf[i] = (unsigned char)(random() % 4 ? 'a' + random() % 3 : 0);
            // strings end near page ends too (page-crossing guard)
            const u64 a = random() % 2 ? 0x1000 - 1 - random() % 24 : random() % 0xf000, b = random() % 2 ? 0x2000 - 1 - random() % 24 : random() % 0xf000;
            buf[0xffff] = 0;
            const size_t n = random() % 4 ? random() % 40 : random() % 600;
            runs++;
            {  // strlen
                u64 r = t.call(at[5], (u64[]){(u64)(buf + a)}, 1).rax;
                if (r != strlen((char *)buf + a)) { bad++; fprintf(stderr, "strlen mismatch at %#lx\n", (unsigned long)a); }
            }
            {  // strcmp: make b a near copy of a sometimes
                if (random() % 2) { size_t l = strlen((char *)buf + a); if (b + l + 1 < 0xffff && (b > a + l || b + l < a)) { memcpy(buf + b, buf + a, l + 1); if (l && random() % 2) buf[b + random() % l] ^= 1; } }
                long r = (int)t.call(at[3], (u64[]){(u64)(buf + a), (u64)(buf + b)}, 2).rax;
                if (sgn(r) != sgn(strcmp((char *)buf + a, (char *)buf + b))) { bad++; fprintf(stderr, "strcmp mismatch %#lx %#lx\n", (unsigned long)a, (unsigned long)b); }
            }
            if (a + n < 0x10000 && b + n < 0x10000) {  // memcmp
                long r = (int)t.call(at[4], (u64[]){(u64)(buf + a), (u64)(buf + b), n}, 3).rax;
                if (sgn(r) != sgn(memcmp(buf + a, buf + b, n))) { bad++; fprintf(stderr, "memcmp mismatch %#lx %#lx %zu\n", (unsigned long)a, (unsigned long)b, n); }
                // memmove (overlapping both ways) against glibc on a copy
                memcpy(ref, buf, 0x10000);
                memmove(ref + b, ref + a, n);
                u64 r2 = t.call(at[0], (u64[]){(u64)(buf + b), (u64)(buf + a), n}, 3).rax;
                if (r2 != (u64)(buf + b) || memcmp(ref, buf, 0x10000)) { bad++; fprintf(stderr, "memmove mismatch %#lx %#lx %zu\n", (unsigned long)b, (unsigned long)a, n); }
                memset(ref + a, 0, n);
                t.call(at[2], (u64[]){(u64)(buf + a), n}, 2);
                if (memcmp(ref, buf, 0x10000)) { bad++; fprintf(stderr, "bzero mismatch %#lx %zu\n", (unsigned long)a, n); }
            }
            {  // Perf 15: strncmp, memchr, strchr, strnlen
                const size_t m = random() % 48;
                long r = (int)t.call(at[6], (u64[]){(u64)(buf + a), (u64)(buf + b), m}, 3).rax;
                if (sgn(r) != sgn(strncmp((char *)buf + a, (char *)buf + b, m))) { bad++; fprintf(stderr, "strncmp mismatch %#lx %#lx %zu\n", (unsigned long)a, (unsigned long)b, m); }
                const int c = random() % 2 ? 'a' + random() % 3 : 0;
                if (a + n < 0x10000) {
                    u64 r2 = t.call(at[7], (u64[]){(u64)(buf + a), (u64)c, n}, 3).rax;
                    if (r2 != (u64)memchr(buf + a, c, n)) { bad++; fprintf(stderr, "memchr mismatch %#lx %d %zu\n", (unsigned long)a, c, n); }
                }
                u64 r3 = t.call(at[8], (u64[]){(u64)(buf + a), (u64)c}, 2).rax;
                if (r3 != (u64)strchr((char *)buf + a, c)) { bad++; fprintf(stderr, "strchr mismatch %#lx %d\n", (unsigned long)a, c); }
                u64 r4 = t.call(at[9], (u64[]){(u64)(buf + a), m}, 2).rax;
                if (r4 != strnlen((char *)buf + a, m)) { bad++; fprintf(stderr, "strnlen mismatch %#lx %zu\n", (unsigned long)a, m); }
            }
            if (bad > 10) break;
        }
        fprintf(stderr, "guestlib test: %d iterations, %d mismatches\n", runs, bad);
    }
    exit(0);
}

}  // namespace a64
