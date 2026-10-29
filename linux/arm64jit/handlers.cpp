// libarm64jit: libsystem_kernel/platform/pthread exports that the generic pass-through can't serve: variadic
// functions, function-pointer arguments (native -> guest trampolines), bool results, Darling names that differ from
// the arm64 ones (64-bit-inode stat), the setjmp family (guest code) and the guest-side init functions (no-ops: the
// native libraries were initialized by the host program's own libSystem).
#include "a64.h"
#include <cstdio>
#include <atomic>
#include <map>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <asm/prctl.h>

namespace a64 {

static u64 cb(u64 guest_fn) { return guest_fn > 1 ? (u64)guest_callable(guest_fn) : guest_fn; }  // keep SIG_DFL/IGN
static u64 uncb(u64 native) { u64 g = guest_of_callable((const void *)native); return g ? g : native; }

// Calls the thunk's native function with the guest's integer args after translating some of them.
template <int... Fn> static void h_callbacks(GuestThread &t, u32 i) {
    u64 ints[16];
    for (int k = 0; k < 8; k++) ints[k] = t.x(k);
    for (int k = 8; k < 16; k++) ints[k] = t.stack_slot(k - 8);
    ((ints[Fn] = cb(ints[Fn])), ...);
    u64 fps[8] = {}; NRet r;
    a64_ncall(g_thunks[i].native, ints, fps, &r);
    t.set_x(0, r.rax);
}

static void h_pthread_create_with_workgroup(GuestThread &t, u32) {  // (thread, workgroup, attr, start, arg)
    static auto *fn = (int (*)(u64, u64, const void *, u64))dsym("pthread_create");
    t.set_x(0, (u32)fn(t.x(0), t.x(2), guest_callable(t.x(3)), t.x(4)));
}

static void h_introspection_hook(GuestThread &t, u32 i) {
    auto *fn = (u64(*)(u64))g_thunks[i].native;
    t.set_x(0, uncb(fn(cb(t.x(0)))));
}

static void h_workqueue_setup(GuestThread &t, u32 i) {  // (struct pthread_workqueue_config *, size)
    struct Cfg { uint32_t flags, version; u64 kevent_cb, workloop_cb, workq_cb, serialno_offs, label_offs; } c{};
    size_t n = t.x(1) < sizeof c ? t.x(1) : sizeof c;
    memcpy(&c, (void *)t.x(0), n);
    c.kevent_cb = cb(c.kevent_cb); c.workloop_cb = cb(c.workloop_cb); c.workq_cb = cb(c.workq_cb);
    auto *fn = (int (*)(Cfg *, size_t))g_thunks[i].native;
    t.set_x(0, (u32)fn(&c, t.x(1)));
}

// int __sigaction(int, struct __sigaction *{handler, tramp, mask, flags}, struct sigaction *{handler, mask, flags})
static void h_sigaction(GuestThread &t, u32 i) {
    struct NSA { u64 handler, tramp; uint32_t mask; int flags; } nsa;
    struct OSA { u64 handler; uint32_t mask; int flags; } *osa = (OSA *)t.x(2);
    NSA *p = nullptr;
    if (t.x(1)) {
        nsa = *(NSA *)t.x(1);
        nsa.handler = cb(nsa.handler);
        static u64 tramp = (u64)dsym("_sigtramp");
        if (tramp) nsa.tramp = tramp;
        p = &nsa;
    }
    auto *fn = (int (*)(int, NSA *, OSA *))g_thunks[i].native;
    int r = fn((int)t.x(0), p, osa);
    if (r == 0 && osa) osa->handler = uncb(osa->handler);
    t.set_x(0, (u32)r);
}

// _simple_* formatters (libsystem_platform's async-signal-safe printf).
static void simple_out(GuestThread &t, int fd, const std::string &s) {
    (void)t;
    if (write(fd, s.data(), s.size()) < 0) {}
}
static void h_simple_dprintf(GuestThread &t, u32) { simple_out(t, (int)t.x(0), guest_format((const char *)t.x(1), (const u64 *)t.sp())); }
static void h_simple_vdprintf(GuestThread &t, u32) { simple_out(t, (int)t.x(0), guest_format((const char *)t.x(1), (const u64 *)t.x(2))); }
static int sappend(u64 b, const std::string &s) {
    static auto *fn = (int (*)(u64, const char *))dsym("_simple_sappend");
    return fn(b, s.c_str());
}
static void h_simple_sprintf(GuestThread &t, u32) { t.set_x(0, (u32)sappend(t.x(0), guest_format((const char *)t.x(1), (const u64 *)t.sp()))); }
static void h_simple_vsprintf(GuestThread &t, u32) { t.set_x(0, (u32)sappend(t.x(0), guest_format((const char *)t.x(1), (const u64 *)t.x(2)))); }
static void h_simple_esprintf(GuestThread &t, u32) { t.set_x(0, (u32)sappend(t.x(0), guest_format((const char *)t.x(2), (const u64 *)t.sp()))); }
static void h_simple_vesprintf(GuestThread &t, u32) { t.set_x(0, (u32)sappend(t.x(0), guest_format((const char *)t.x(2), (const u64 *)t.x(3)))); }


// ---- VM: guest pages are 16K, and libmalloc asks for aligned regions (mask); Darling's maps are 4K-aligned and
// ignore the mask. An "anywhere" request reserves size + alignment, then maps fixed+overwrite at the aligned address.
// Also: >8-argument functions pass their 9th+ int arguments packed by natural size on the Apple arm64 stack.
constexpr int kAnywhere = 1, kOverwrite = 0x4000;
// Guest memory also has to stay below 64 GB (iOS's user address space; xzone malloc's segment table covers no more):
// requests are placed first-fit from a cursor in [12 GB, 63 GB), above the shared cache, below the commpage.
static std::atomic<u64> g_low_cursor{0x300000000ull};
constexpr u64 kLowEnd = 0xFC0000000ull;
static u64 reserve_low(u64 len) {
    for (int wrapped = 0; wrapped < 2; wrapped++) {
        for (;;) {
            u64 at = g_low_cursor.load();
            if (at + len > kLowEnd) break;
            void *p = mmap((void *)at, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == (void *)at) { u64 want = at; g_low_cursor.compare_exchange_strong(want, at + len); return at; }
            if (p != MAP_FAILED) munmap(p, len);
            u64 want = at; g_low_cursor.compare_exchange_strong(want, at + 0x100000);  // taken: step 1 MB
        }
        g_low_cursor = 0x300000000ull;
    }
    return 0;
}
u64 reserve_aligned(u64 size, u64 mask) {
    u64 align = (mask | 0x3fff) + 1;
    u64 r = reserve_low(size + align);
    if (!r) return 0;
    u64 a = (r + align - 1) & ~(align - 1);
    if (a > r) munmap((void *)r, a - r);
    u64 end = r + size + align, aend = a + ((size + 0x3fff) & ~0x3fffull);
    if (end > aend) munmap((void *)aend, end - aend);
    return a;
}
// kern_return_t f(task, addr *, size, mask, flags, ...rest) with rest as 64-bit native args
static void vm_map_like(GuestThread &t, u32 i, u64 *ints, int addr_arg, int size_arg, int mask_arg, int flags_arg) {
    u64 *addrp = (u64 *)ints[addr_arg];
    u64 fps[8] = {}; NRet r;
    if ((ints[flags_arg] & kAnywhere) && addrp) {
        u64 a = reserve_aligned(ints[size_arg], mask_arg >= 0 ? ints[mask_arg] : 0);
        if (a) {
            u64 saved = *addrp; *addrp = a;
            u64 f = ints[flags_arg]; ints[flags_arg] = (f & ~(u64)kAnywhere) | kOverwrite;
            a64_ncall(g_thunks[i].native, ints, fps, &r);
            if ((int)r.rax == 0) { t.set_x(0, 0); return; }
            munmap((void *)a, (ints[size_arg] + 0x3fff) & ~0x3fffull);
            *addrp = saved; ints[flags_arg] = f;
        }
    }
    a64_ncall(g_thunks[i].native, ints, fps, &r);
    t.set_x(0, (u32)r.rax);
}
static void load_regs(GuestThread &t, u64 *ints) { for (int k = 0; k < 16; k++) ints[k] = k < 8 ? t.x(k) : 0; }
// (mach_)vm_map(task, addr*, size, mask, flags, object, offset, copy | int cur, int max, uint inherit)
static void h_vm_map(GuestThread &t, u32 i) {
    u64 ints[16]; load_regs(t, ints);
    u64 sp = t.sp();
    ints[8] = GuestThread::rd<int32_t>(sp); ints[9] = GuestThread::rd<int32_t>(sp + 4); ints[10] = GuestThread::rd<uint32_t>(sp + 8);
    vm_map_like(t, i, ints, 1, 2, 3, 4);
}
// (mach_)vm_remap[_new](task, addr*, size, mask, flags, src_task, src_addr, copy | cur*, max*, uint inherit)
static void h_vm_remap(GuestThread &t, u32 i) {
    u64 ints[16]; load_regs(t, ints);
    u64 sp = t.sp();
    ints[8] = GuestThread::rd<u64>(sp); ints[9] = GuestThread::rd<u64>(sp + 8); ints[10] = GuestThread::rd<uint32_t>(sp + 16);
    vm_map_like(t, i, ints, 1, 2, 3, 4);
}
// (mach_)vm_allocate(task, addr*, size, flags)
static void h_vm_allocate(GuestThread &t, u32 i) {
    u64 ints[16]; load_regs(t, ints);
    vm_map_like(t, i, ints, 1, 2, -1, 3);
}
// mmap(addr, len, prot, flags, fd, off): non-fixed maps 16K-aligned
static void h_mmap(GuestThread &t, u32 i) {
    u64 ints[16]; load_regs(t, ints);
    u64 fps[8] = {}; NRet r;
    constexpr u64 kFixed = 0x10;
    if (!(ints[3] & kFixed) && ints[1]) {
        if (u64 a = reserve_aligned(ints[1], 0)) {
            u64 save0 = ints[0], save3 = ints[3];
            ints[0] = a; ints[3] |= kFixed;
            a64_ncall(g_thunks[i].native, ints, fps, &r);
            if (r.rax == a) { t.set_x(0, a); return; }
            munmap((void *)a, (ints[1] + 0x3fff) & ~0x3fffull);
            ints[0] = save0; ints[3] = save3;
        }
    }
    a64_ncall(g_thunks[i].native, ints, fps, &r);
    t.set_x(0, r.rax);
}
// mach_msg_overwrite(msg, option, send_size, rcv_size, rcv_name, timeout, notify, rcv_msg | uint rcv_limit)
static void h_msg_overwrite(GuestThread &t, u32 i) {
    u64 ints[16]; load_regs(t, ints);
    ints[8] = GuestThread::rd<uint32_t>(t.sp());
    u64 fps[8] = {}; NRet r;
    a64_ncall(g_thunks[i].native, ints, fps, &r);
    t.set_x(0, (u32)r.rax);
}

// Generic >8-argument functions: the 9th+ arguments are packed by natural size/alignment on the Apple arm64 stack
// (each 8-byte slot on SysV). Sizes are given for each stack argument (manyargs audit, ARM64-PLAN stage 2).
template <int... Sz> static void h_stack_args(GuestThread &t, u32 i) {
    u64 ints[16]; load_regs(t, ints);
    u64 off = 0, sp = t.sp(); int k = 8;
    auto take = [&](int sz) {
        off = (off + sz - 1) & ~(u64)(sz - 1);
        ints[k++] = sz == 8 ? GuestThread::rd<u64>(sp + off) : sz == 4 ? GuestThread::rd<uint32_t>(sp + off) : GuestThread::rd<uint16_t>(sp + off);
        off += sz;
    };
    (take(Sz), ...);
    u64 fps[8] = {}; NRet r;
    a64_ncall(g_thunks[i].native, ints, fps, &r);
    t.set_x(0, r.rax);
}
// Structs over 16 bytes by value: arm64 passes a pointer, SysV copies the struct onto the stack.
// task_register_dyld_shared_cache_image_info(task, dyld_kernel_image_info_t (40 B), bool, bool)
static void h_dyld_cache_info(GuestThread &t, u32 i) {
    u64 ints[16] = {t.x(0), t.x(2), t.x(3), 0, 0, 0};
    memcpy(&ints[6], (void *)t.x(1), 40);
    u64 fps[8] = {}; NRet r;
    a64_ncall(g_thunks[i].native, ints, fps, &r);
    t.set_x(0, (u32)r.rax);
}

// libSystem's initializer calls this first, before __malloc_init: the cache is mapped, nothing has allocated yet.
static void h_libkernel_init(GuestThread &t, u32) { native_malloc_install(); t.set_x(0, 0); }

void print_stats();
static void h_exit(GuestThread &t, u32 i) {  // __exit: the guest process ends here
    print_stats();
    ((void (*)(int))g_thunks[i].native)((int)t.x(0));
}

Handler special_handler(const std::string &n, int *nfixed, const char **native_name) {
    static const std::map<std::string, int> variadic = {
        {"_open", 2}, {"_open$NOCANCEL", 2}, {"_openat", 3}, {"_openat$NOCANCEL", 3}, {"_fcntl", 2}, {"_fcntl$NOCANCEL", 2},
        {"_ioctl", 2}, {"_syscall", 1}, {"_semctl", 3}, {"_sem_open", 2}, {"_shm_open", 2}, {"_msgsys", 1}, {"_semsys", 1},
        {"_shmsys", 1}, {"_open_dprotected_np", 4}, {"_openat_dprotected_np", 5}, {"_guarded_open_np", 4},
        {"_guarded_open_dprotected_np", 6}, {"_panic", 1},
    };
    if (auto it = variadic.find(n); it != variadic.end()) { *nfixed = it->second; return h_variadic; }
    // Darling (x86_64) keeps the 32-bit-inode structs under the plain names; arm64 only has the 64-bit ones.
    static const std::map<std::string, const char *> renamed = {
        {"_stat", "stat$INODE64"}, {"_fstat", "fstat$INODE64"}, {"_lstat", "lstat$INODE64"}, {"_fstatat", "fstatat$INODE64"},
        {"_statfs", "statfs$INODE64"}, {"_fstatfs", "fstatfs$INODE64"}, {"_getfsstat", "getfsstat$INODE64"},
        {"___stat", "stat$INODE64"}, {"___fstat", "fstat$INODE64"}, {"___lstat", "lstat$INODE64"},
        {"___fstatat", "fstatat$INODE64"}, {"___statfs", "statfs$INODE64"}, {"___fstatfs", "fstatfs$INODE64"},
        {"___getfsstat", "getfsstat$INODE64"}, {"_getdirentries", "__getdirentries64"},
        {"_mach_absolute_time_kernel", "mach_absolute_time"}, {"_mach_continuous_time_kernel", "mach_continuous_time"},
        {"_cthread_yield", "sched_yield"},
    };
    if (auto it = renamed.find(n); it != renamed.end()) { *native_name = it->second; return h_generic; }
    static const std::map<std::string, Handler> special = {
        {"____chkstk_darwin", h_noop}, {"_thread_chkstk_darwin", h_noop},
        {"___libkernel_init", h_libkernel_init}, {"___libkernel_platform_init", h_zero}, {"___libkernel_voucher_init", h_zero},
        {"___libkernel_init_after_boot_tasks", h_zero}, {"___libplatform_init", h_zero}, {"___pthread_init", h_zero},
        {"___pthread_late_init", h_zero}, {"_mach_init", h_zero}, {"__pthread_yield_to_enqueuer_4dispatch", h_zero},
        {"__pthread_mutex_enable_legacy_mode", h_zero}, {"_pthread_prefer_alternate_cluster_self", h_zero},
        {"_pthread_create", h_callbacks<2>}, {"_pthread_create_suspended_np", h_callbacks<2>},
        {"_pthread_create_from_mach_thread", h_callbacks<2>}, {"_pthread_create_with_workgroup_np", h_pthread_create_with_workgroup},
        {"_pthread_once", h_callbacks<1>}, {"_pthread_atfork", h_callbacks<0, 1, 2>},
        {"_pthread_key_create", h_callbacks<1>}, {"_pthread_key_init_np", h_callbacks<1>},
        {"__os_once", h_callbacks<2>}, {"__os_alloc_once", h_callbacks<2>},
        {"__pthread_workqueue_init", h_callbacks<0>}, {"__pthread_workqueue_init_with_kevent", h_callbacks<0, 1>},
        {"__pthread_workqueue_init_with_workloop", h_callbacks<0, 1, 2>}, {"_pthread_workqueue_setdispatch_np", h_callbacks<0>},
        {"_pthread_workqueue_setup", h_workqueue_setup}, {"_pthread_introspection_hook_install", h_introspection_hook},
        {"_mach_msg_server", h_callbacks<0>}, {"_mach_msg_server_once", h_callbacks<0>},
        {"_mach_msg_server_importance", h_callbacks<0>}, {"___sigaction", h_sigaction},
        {"_mach_vm_map", h_vm_map}, {"_vm_map", h_vm_map}, {"_mach_vm_remap", h_vm_remap}, {"_vm_remap", h_vm_remap},
        {"_mach_vm_remap_new", h_vm_remap}, {"_vm_remap_new", h_vm_remap}, {"_mach_vm_allocate", h_vm_allocate},
        {"_vm_allocate", h_vm_allocate}, {"_mmap", h_mmap}, {"___mmap", h_mmap}, {"_mach_msg_overwrite", h_msg_overwrite},
        {"__exit", h_exit}, {"___exit", h_exit},
        {"_exception_raise_state", h_stack_args<8>}, {"_exception_raise_state_identity", h_stack_args<4, 8, 8>},
        {"_host_swap_exception_ports", h_stack_args<8, 8>}, {"_task_swap_exception_ports", h_stack_args<8, 8>},
        {"_thread_swap_exception_ports", h_stack_args<8, 8>}, {"_kext_request", h_stack_args<8>},
        {"_mach_msg_overwrite_trap", h_stack_args<4>}, {"_task_register_dyld_shared_cache_image_info", h_dyld_cache_info},
        // security_token_t (8 B) + audit_token_t (32 B) by value: not used by the simulator, Darling has no host_security
        {"_host_security_create_task_token", h_missing}, {"_host_security_set_task_token", h_missing},
        {"__simple_dprintf", h_simple_dprintf}, {"__simple_vdprintf", h_simple_vdprintf},
        {"__simple_sprintf", h_simple_sprintf}, {"__simple_vsprintf", h_simple_vsprintf},
        {"__simple_esprintf", h_simple_esprintf}, {"__simple_vesprintf", h_simple_vesprintf},
    };
    if (auto it = special.find(n); it != special.end()) return it->second;
    // bool results: x86 callees only define al; arm64 callers expect w0 zero-extended.
    if (n.find("CompareAndSwap") != std::string::npos || n.find("TestAnd") != std::string::npos ||
        n.find("trylock") != std::string::npos || n.find("Try") != std::string::npos || n == "_kdebug_is_enabled" ||
        n == "_kdebug_using_continuous_time")
        return h_generic_bool;
    return nullptr;
}

// ---- setjmp family as guest code -----------------------------------------------------------------------------------
// jmp_buf (ours, private to these functions): x19-x28, x29, x30, sp, d8-d15. The signal mask is not saved.
// pthread_getspecific/setspecific: guest code on the native TSD (guest tpidrro_el0 = Darling's x86_64 TSD base, see
// jit.cpp). setspecific stores directly when libpthread's bookkeeping needs no update (key in [10, max_tsd_key]);
// otherwise it takes the thunk. max_tsd_key is at TSD base - 148 in Darling's x86_64 struct pthread_s (tsd at +224).
static bool tsd_layout_ok() {
    static int ok = -1;
    if (ok < 0) {
        unsigned long gs = 0;
        syscall(SYS_arch_prctl, ARCH_GET_GS, &gs);
        static auto *self = (u64 (*)())dsym("pthread_self");
        ok = gs && self && *(u64 *)gs == self() && self() == gs - 224;
        if (!ok) logf("TSD layout unexpected: pthread_getspecific/setspecific stay thunks");
    }
    return ok;
}
bool is_guest_code_export(const std::string &n) {
    if (n == "_pthread_getspecific" || n == "_pthread_setspecific") return tsd_layout_ok();
    return n == "_setjmp" || n == "__setjmp" || n == "_sigsetjmp" || n == "_longjmp" || n == "__longjmp" || n == "_siglongjmp";
}
size_t emit_guest_code(const std::string &n, uint32_t *o, u32 slow) {
    if (n == "_pthread_getspecific") {
        static const uint32_t c[] = {0xD53BD068, 0xF8607900, 0xD65F03C0};  // mrs x8, tpidrro_el0; ldr x0, [x8, x0, lsl #3]; ret
        memcpy(o, c, sizeof c);
        return 3;
    }
    if (n == "_pthread_setspecific") {
        static const uint32_t c[] = {
            0xD53BD068,  // mrs x8, tpidrro_el0
            0x7856C109,  // ldurh w9, [x8, #-148]   (max_tsd_key)
            0xEB09001F,  // cmp x0, x9
            0x540000C8,  // b.hi slow (+6 insns)
            0xF100281F,  // cmp x0, #10
            0x54000083,  // b.lo slow (+4)
            0xF8207901,  // str x1, [x8, x0, lsl #3]
            0x52800000,  // mov w0, #0
            0xD65F03C0,  // ret
        };
        memcpy(o, c, sizeof c);
        emit_stub(o + 9, slow);  // slow: svc #slow; ret
        return 11;
    }
    static const uint32_t setjmp_code[] = {
        0xA9005013,  // stp x19, x20, [x0]
        0xA9015815,  // stp x21, x22, [x0, #16]
        0xA9026017,  // stp x23, x24, [x0, #32]
        0xA9036819,  // stp x25, x26, [x0, #48]
        0xA904701B,  // stp x27, x28, [x0, #64]
        0xA905781D,  // stp x29, x30, [x0, #80]
        0x910003E1,  // mov x1, sp
        0xF9003001,  // str x1, [x0, #96]
        0x6D07A408,  // stp d8, d9, [x0, #120]
        0x6D08AC0A,  // stp d10, d11, [x0, #136]
        0x6D09B40C,  // stp d12, d13, [x0, #152]
        0x6D0ABC0E,  // stp d14, d15, [x0, #168]
        0x52800000,  // mov w0, #0
        0xD65F03C0,  // ret
    };
    static const uint32_t longjmp_code[] = {
        0xA9405013,  // ldp x19, x20, [x0]
        0xA9415815,  // ldp x21, x22, [x0, #16]
        0xA9426017,  // ldp x23, x24, [x0, #32]
        0xA9436819,  // ldp x25, x26, [x0, #48]
        0xA944701B,  // ldp x27, x28, [x0, #64]
        0xA945781D,  // ldp x29, x30, [x0, #80]
        0xF9403002,  // ldr x2, [x0, #96]
        0x9100005F,  // mov sp, x2
        0x6D47A408,  // ldp d8, d9, [x0, #120]
        0x6D48AC0A,  // ldp d10, d11, [x0, #136]
        0x6D49B40C,  // ldp d12, d13, [x0, #152]
        0x6D4ABC0E,  // ldp d14, d15, [x0, #168]
        0x7100003F,  // cmp w1, #0
        0x1A9F1420,  // csinc w0, w1, wzr, ne
        0xD65F03C0,  // ret
    };
    bool lj = n.find("longjmp") != std::string::npos;
    const uint32_t *src = lj ? longjmp_code : setjmp_code;
    size_t cnt = lj ? sizeof longjmp_code / 4 : sizeof setjmp_code / 4;
    memcpy(o, src, cnt * 4);
    return cnt;
}

}  // namespace a64
