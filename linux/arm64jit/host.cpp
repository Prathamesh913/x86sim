#include <algorithm>
// libarm64jit: process setup. Called by the x86_64 host program (host/arm64host) after Darling's libSystem is up.
// Maps the arm64 main executable and dyld_sim, writes the 16K arm64 commpage, builds the stub dylibs for the host
// libraries, then runs _dyld_sim_prepare() and main under the JIT on this (the main) thread. dyld_sim maps the
// runtime's prebuilt shared cache itself (fixed at 0x1C0000000) through the SyscallHelpers table we hand it.
#include "a64.h"
#include "macho.h"
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <execinfo.h>
#include <dlfcn.h>

namespace a64 {
bool g_step = getenv("X86CODE_A64_STEP") != nullptr;  // one instruction per Run (exact fault PCs)

const DarwinApi *g_darwin;
void *dsym(const char *name) { return g_darwin->dlsym((void *)-2 /* RTLD_DEFAULT */, name); }

static int (*d_open)(const char *, int, ...);
static timespec g_t0;
static double ms_since(const timespec &a) { timespec b; clock_gettime(CLOCK_MONOTONIC, &b); return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6; }

// ---- Mach-O mapping ----------------------------------------------------------------------------------------------
struct Mapped { u64 mh = 0, slide = 0, entry = 0; };

static Mapped map_macho(const std::string &path, bool want_unixthread) {
    int fd = d_open(path.c_str(), O_RDONLY);
    if (fd < 0 && path[0] == '/' && path.compare(0, 20, "/Volumes/SystemRoot/"))  // a Linux path from mldr
        fd = d_open(("/Volumes/SystemRoot" + path).c_str(), O_RDONLY);
    if (fd < 0) die("cannot open %s (errno %d)", path.c_str(), *g_darwin->error());
    struct stat st; fstat(fd, &st);
    auto *file = (const uint8_t *)mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED) die("cannot map %s", path.c_str());
    u64 slice = 0;
    uint32_t magic; memcpy(&magic, file, 4);
    if (magic == 0xbebafecau) {  // fat: the arm64 slice
        uint32_t n = __builtin_bswap32(*(const uint32_t *)(file + 4));
        bool found = false;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *a = file + 8 + i * 20;
            if (__builtin_bswap32(*(const uint32_t *)a) == (uint32_t)CPU_TYPE_ARM64) { slice = __builtin_bswap32(*(const uint32_t *)(a + 8)); found = true; }
        }
        if (!found) die("%s has no arm64 slice", path.c_str());
    }
    auto *mh = (const mach_header_64 *)(file + slice);
    if (mh->magic != MH_MAGIC_64 || mh->cputype != CPU_TYPE_ARM64) die("%s is not an arm64 Mach-O", path.c_str());
    if ((mh->cpusubtype & 0xff) == 2) die("%s is arm64e (not supported)", path.c_str());
    u64 lo = ~0ull, hi = 0, pc = 0, entryoff = 0;
    const uint8_t *lc = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const load_command *)lc)->cmdsize) {
        auto *sg = (const segment_command_64 *)lc;
        if (sg->cmd == LC_SEGMENT_64 && !(sg->initprot == 0 && sg->filesize == 0)) {
            lo = std::min(lo, sg->vmaddr); hi = std::max(hi, sg->vmaddr + sg->vmsize);
        } else if (sg->cmd == LC_UNIXTHREAD) {
            pc = *(const u64 *)(lc + 16 + 32 * 8);  // ARM_THREAD_STATE64: x0-x28, fp, lr, sp, pc
        } else if (sg->cmd == LC_MAIN) entryoff = ((const entry_point_command *)lc)->entryoff;
    }
    // Prefer the link address (slide 0); otherwise anywhere.
    u64 size = hi - lo;
    void *base = mmap((void *)lo, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (lo ? MAP_FIXED_NOREPLACE : 0), -1, 0);
    if (base == MAP_FAILED) base = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) die("no address space for %s", path.c_str());
    Mapped m; m.slide = (u64)base - lo;
    lc = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const load_command *)lc)->cmdsize) {
        auto *sg = (const segment_command_64 *)lc;
        if (sg->cmd != LC_SEGMENT_64 || (sg->initprot == 0 && sg->filesize == 0)) continue;
        int prot = (sg->initprot & 1 ? PROT_READ : 0) | (sg->initprot & 2 ? PROT_WRITE : 0);  // guest code is never host-executed
        if (!prot) prot = PROT_READ;
        if (sg->filesize) {
            void *p = mmap((void *)(sg->vmaddr + m.slide), sg->filesize, prot | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, slice + sg->fileoff);
            if (p == MAP_FAILED) die("cannot map %s %s", path.c_str(), sg->segname);
        }
        if (sg->vmsize > sg->filesize) {
            u64 a = (sg->vmaddr + m.slide + sg->filesize + 4095) & ~4095ull, e = sg->vmaddr + m.slide + sg->vmsize;
            if (e > a) mmap((void *)a, e - a, prot | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        }
        if (sg->fileoff == 0 && sg->filesize) m.mh = sg->vmaddr + m.slide;
    }
    munmap((void *)file, st.st_size);
    close(fd);
    m.entry = want_unixthread ? pc + m.slide : (entryoff ? m.mh + entryoff : 0);
    return m;
}

// ---- commpage ------------------------------------------------------------------------------------------------------
static void map_commpage() {
    constexpr u64 kBase = 0xFFFFFC000ull;
    auto *p = (uint8_t *)mmap((void *)kBase, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (uint8_t *)kBase) { logf("warning: cannot map the arm64 commpage at %#lx", (unsigned long)kBase); return; }
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > 255) ncpu = 255;
    memcpy(p, "commpage 64-bit", 16);
    u64 caps = 0x00000020 /* kCache64 */ | 0x100 /* kHasNeon */ | 0x400 /* kHasVfp */ | 0x2000 /* kHasFMA */ |
               0x01000000 /* kHasARMv8Crypto */ | 0x02000000 /* kHasARMv81Atomics */ | 0x04000000 /* kHasARMv8Crc32 */ |
               ((u64)ncpu << 16);
    memcpy(p + 0x10, &caps, 8);
    uint16_t ver = 3; memcpy(p + 0x1E, &ver, 2);
    uint32_t caps32 = (uint32_t)caps; memcpy(p + 0x20, &caps32, 4);
    p[0x22] = (uint8_t)ncpu;
    p[0x24] = 14; p[0x25] = 14;  // user page shift 32/64: 16K
    uint16_t line = 64; memcpy(p + 0x26, &line, 2);
    p[0x34] = p[0x35] = p[0x36] = (uint8_t)ncpu;
    p[0x37] = 14;
    u64 mem = (u64)sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE); memcpy(p + 0x38, &mem, 8);
    uint32_t family = 0x1b588bb3; memcpy(p + 0x80, &family, 4);  // CPUFAMILY_ARM_FIRESTORM_ICESTORM (M1)
    mprotect(p, 0x4000, PROT_READ);
}

// ---- SyscallHelpers (dyld_sim's view of the host dyld) -------------------------------------------------------------
static void h_helper_open(GuestThread &t, u32) {
    const char *path = (const char *)t.x(0);
    int fd = stub_fd_for_path(path);
    if (fd < 0) fd = d_open(path, (int)t.x(1), (int)t.stack_slot(0));
    if (g_trace) logf("dyld_sim open %s -> %d", path, fd);
    t.set_x(0, (u64)(int64_t)fd);
}
static void h_helper_stat(GuestThread &t, u32) {
    static auto *dstat = (int (*)(const char *, void *))dsym("stat$INODE64");
    static auto *dfstat = (int (*)(int, void *))dsym("fstat$INODE64");
    int fd = stub_fd_for_path((const char *)t.x(0));
    int r;
    if (fd >= 0) { r = dfstat(fd, (void *)t.x(1)); close(fd); }
    else r = dstat((const char *)t.x(0), (void *)t.x(1));
    t.set_x(0, (u64)(int64_t)r);
}
static void h_helper_vlog(GuestThread &t, u32) {
    std::string s = guest_format((const char *)t.x(0), (const u64 *)t.x(1));
    if (write(2, s.data(), s.size()) < 0) {}
}
static u64 g_all_image_infos[512];
static void h_helper_process_info(GuestThread &t, u32) {
    if (!g_all_image_infos[0]) g_all_image_infos[0] = 17;  // version
    t.set_x(0, (u64)g_all_image_infos);
}
static void h_helper_amfi(GuestThread &t, u32) {  // amfi_check_dyld_policy_self(in, &out): allow everything
    if (t.x(1)) *(u64 *)t.x(1) = 0x7f;
    t.set_x(0, 0);
}
static void h_helper_notify_get(GuestThread &t, u32 i) {
    if (g_thunks[i].native) { h_generic(t, i); return; }
    if (t.x(1)) *(uint32_t *)t.x(1) = 0;
    t.set_x(0, 0);
}

static u64 build_syscall_helpers() {
    struct H { const char *native; Handler h; };
    static const H table[] = {
        {"open", h_helper_open}, {"close", nullptr}, {"pread", nullptr}, {"write", nullptr}, {"mmap", nullptr},
        {"munmap", nullptr}, {"madvise", nullptr}, {"stat", h_helper_stat}, {"fcntl", h_variadic}, {"ioctl", h_variadic},
        {"issetugid", nullptr}, {"getcwd", nullptr}, {"realpath$DARWIN_EXTSN", nullptr}, {"vm_allocate", nullptr},
        {"vm_deallocate", nullptr}, {"vm_protect", nullptr}, {"vlog", h_helper_vlog}, {"vwarn", h_helper_vlog},
        {"pthread_mutex_lock", nullptr}, {"pthread_mutex_unlock", nullptr}, {"mach_thread_self", nullptr},
        {"mach_port_deallocate", nullptr}, {"task_self_trap", nullptr}, {"mach_timebase_info", nullptr},
        {"OSAtomicCompareAndSwapPtrBarrier", h_generic_bool}, {"OSMemoryBarrier", nullptr},
        {"getProcessInfo", h_helper_process_info}, {"__error", nullptr}, {"mach_absolute_time", nullptr},
        {"thread_switch", nullptr},
        {"opendir_fake", h_zero}, {"readdir_r", h_zero}, {"closedir", h_zero},  // version 3 (unused)
        {"cs_load_notifier", h_noop}, {"cs_unload_notifier", h_noop},
        {"proc_regionfilename", nullptr}, {"getpid", nullptr}, {"mach_port_insert_right", nullptr},
        {"mach_port_allocate", nullptr}, {"mach_msg", nullptr}, {"abort_with_payload", nullptr},
        {"task_register_dyld_image_infos", nullptr}, {"task_unregister_dyld_image_infos", nullptr},
        {"task_get_dyld_image_infos", nullptr}, {"task_register_dyld_shared_cache_image_info", nullptr},
        {"task_register_dyld_set_dyld_state", nullptr}, {"task_register_dyld_get_process_state", nullptr},
        {"task_info", nullptr}, {"thread_info", nullptr}, {"kdebug_is_enabled", h_generic_bool}, {"kdebug_trace", nullptr},
        {"kdebug_trace_string", nullptr}, {"amfi_check_dyld_policy_self", h_helper_amfi},
        {"notifyMonitorOfMainCalled", h_noop}, {"notifyMonitorOfImageListChanges", h_noop},
        {"mach_msg_destroy", nullptr}, {"mach_port_construct", nullptr}, {"mach_port_destruct", nullptr},
        {"fstat$INODE64", nullptr}, {"vm_copy", nullptr}, {"task_dyld_process_info_notify_get", h_helper_notify_get},
        {"fsgetpath", nullptr}, {"getattrlistbulk", nullptr}, {"getattrlist", nullptr}, {"getfsstat$INODE64", nullptr},
        {"notifyMonitorOfDyldBeforeInitializers", h_noop}, {"sysctlbyname", nullptr},
    };
    constexpr size_t n = sizeof table / sizeof table[0];
    auto *vt = new u64[n + 1];
    vt[0] = 18;
    for (size_t i = 0; i < n; i++) {
        const void *native = dsym(table[i].native);
        Handler h = table[i].h;
        int nfixed = 0;
        if (h == h_variadic) nfixed = 2;
        if (!h) h = native ? h_generic : h_zero;
        vt[i + 1] = stub_code(add_thunk(std::string("dyld_sim helper ") + table[i].native, h, native, nfixed));
    }
    return (u64)vt;
}

// Faults that dynarmic's fastmem handler doesn't recover: report the guest state (last block entry) before Darling's
// handler reports the host side.
static void peek() {
    if (const char *pk = getenv("X86CODE_A64_PEEK"))  // debug: comma-separated guest addresses to dump (8 bytes each)
        for (const char *q = pk; *q;) {
            char *e; u64 a = strtoull(q, &e, 16);
            if (e == q) break;
            logf("peek %#lx: %#lx", (unsigned long)a, (unsigned long)*(u64 *)a);
            q = *e == ',' ? e + 1 : e;
        }
}
static struct sigaction g_old_segv, g_old_bus;
// The cache's CONST_DATA mappings (read-only after dyld's fixups): guest atomics on them fault in fastmem and are
// completed by dynarmic's fallback (ARM64-PLAN stage 2, 1e); those faults are not reported.
static u64 g_const_lo[8], g_const_hi[8];
static int g_nconst;
static void read_const_mappings(const std::string &cache) {
    static const char *suffixes[] = {"", ".01", ".02", ".03", ".04", ".05"};
    for (const char *sfx : suffixes) {
        int fd = d_open((cache + sfx).c_str(), O_RDONLY);
        if (fd < 0) break;
        uint8_t h[0x400]; uint32_t off = 0, cnt = 0;
        if (pread(fd, h, sizeof h, 0) == (ssize_t)sizeof h) { memcpy(&off, h + 0x138, 4); memcpy(&cnt, h + 0x13c, 4); }
        for (uint32_t i = 0; i < cnt && i < 16 && g_nconst < 8; i++) {
            u64 m[7];
            if (pread(fd, m, sizeof m, off + 56 * i) != (ssize_t)sizeof m) break;
            if (m[5] & 4) { g_const_lo[g_nconst] = m[0]; g_const_hi[g_nconst++] = m[0] + m[1]; }
        }
        close(fd);
    }
}
static bool in_const_data(u64 a) {
    for (int i = 0; i < g_nconst; i++) if (a >= g_const_lo[i] && a < g_const_hi[i]) return true;
    return false;
}
static void on_fault(int sig, siginfo_t *si, void *uc) {
    auto *ctx = (ucontext_t *)uc;
    if (sig == SIGSEGV && si->si_code == SEGV_ACCERR && in_const_data((u64)si->si_addr) && !getenv("X86CODE_A64_FAULTLOG")) {
        struct sigaction *old = &g_old_segv;
        if (old->sa_flags & SA_SIGINFO) { old->sa_sigaction(sig, si, uc); return; }
    }
    char buf[256];
    int n = snprintf(buf, sizeof buf, "signal %d code %d addr %p rip %#llx err %#llx\n", sig, si->si_code, si->si_addr,
                     (unsigned long long)ctx->uc_mcontext.gregs[REG_RIP], (unsigned long long)ctx->uc_mcontext.gregs[REG_ERR]);
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = 0;
    logf("%s", buf);
    if (getenv("X86CODE_A64_FAULTBYTES")) {  // debug: the faulting host instruction's first bytes
        auto *ip = (const uint8_t *)ctx->uc_mcontext.gregs[REG_RIP];
        logf("rip bytes %02x %02x %02x %02x %02x %02x", ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
    }
    if (GuestThread *t = GuestThread::current())
        logf("guest pc %#lx lr %#lx sp %#lx x0 %#lx x1 %#lx x8 %#lx", (unsigned long)t->jit->GetPC(), (unsigned long)t->x(30),
             (unsigned long)t->sp(), (unsigned long)t->x(0), (unsigned long)t->x(1), (unsigned long)t->x(8));
    if (GuestThread *t = GuestThread::current(); t && si->si_code != SEGV_ACCERR) { logf("  pc in %s, in thunk %s", where(t->jit->GetPC()).c_str(), t->in_thunk >= 0 ? g_thunks[t->in_thunk].name.c_str() : "-"); guest_backtrace(*t); }
    peek();
    struct sigaction *old = sig == SIGSEGV ? &g_old_segv : &g_old_bus;
    if (old->sa_flags & SA_SIGINFO) old->sa_sigaction(sig, si, uc);
    else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) old->sa_handler(sig);
    else { signal(sig, SIG_DFL); raise(sig); }
}
// SIGABRT from native code (Darling's libsystem, dynarmic asserts): native and guest backtraces, then the default.
static void on_abrt(int, siginfo_t *si, void *) {
    void *bt[32]; int n = backtrace(bt, 32);
    logf("SIGABRT (code %d pid %d) on tid %ld: native backtrace:", si->si_code, si->si_pid, (long)syscall(SYS_gettid));
    for (int i = 0; i < n; i++) {
        Dl_info di{};
        if (dladdr(bt[i], &di) && di.dli_fname) logf("  %p %s+%#lx %s", bt[i], di.dli_fname, (unsigned long)((char *)bt[i] - (char *)di.dli_fbase), di.dli_sname ? di.dli_sname : "");
        else logf("  %p", bt[i]);
    }
    if (GuestThread *t = GuestThread::current()) {
        logf("guest pc %#lx, in thunk %s", (unsigned long)t->jit->GetPC(), t->in_thunk >= 0 ? g_thunks[t->in_thunk].name.c_str() : "-");
        guest_backtrace(*t);
    }
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}
static void install_fault_report() {
    { struct sigaction sa{}; sa.sa_sigaction = on_abrt; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigaction(SIGABRT, &sa, nullptr); }
    struct sigaction sa{};
    sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, &g_old_segv); sigaction(SIGBUS, &sa, &g_old_bus);
}

void print_stats() {
    peek();
    if (!getenv("X86CODE_A64_STATS")) return;
    logf("exit after %.1f ms", ms_since(g_t0));
    extern u64 g_thunk_counts[0x10000];
    std::vector<std::pair<u64, u32>> v;
    for (u32 i = 0; i < g_thunks.size(); i++) if (g_thunk_counts[i]) v.push_back({g_thunk_counts[i], i});
    std::sort(v.rbegin(), v.rend());
    for (size_t k = 0; k < v.size() && k < 12; k++) logf("  %10lu %s", (unsigned long)v[k].first, g_thunks[v[k].second].name.c_str());
}

}  // namespace a64

using namespace a64;

extern "C" __attribute__((visibility("default"))) int a64_host_main(int argc, char **argv, char **envp, char **apple, const DarwinApi *api) {
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    g_darwin = api;
    g_trace = getenv("X86CODE_A64_TRACE") != nullptr;
    // X86CODE_A64_TRACE_PROC=name[,name]: trace only these executables (set for a whole boot)
    if (const char *tp = getenv("X86CODE_A64_TRACE_PROC"); tp && getenv("X86CODE_A64_EXE")) {
        const char *e = getenv("X86CODE_A64_EXE"), *b = strrchr(e, '/');
        std::string base = b ? b + 1 : e, list = std::string(",") + tp + ",";
        g_trace = list.find("," + base + ",") != std::string::npos;
    }
    if (const char *sp = getenv("X86CODE_A64_STEP_PROC"); sp && getenv("X86CODE_A64_EXE")) {
        const char *e = getenv("X86CODE_A64_EXE"), *b = strrchr(e, '/');
        std::string base = b ? b + 1 : e, list = std::string(",") + sp + ",";
        g_step = list.find("," + base + ",") != std::string::npos;
    }
    d_open = (int (*)(const char *, int, ...))dsym("open");
    boundary_init();

    // The guest executable: from mldr (X86CODE_A64_EXE, argv untouched) or "arm64host <exe> [args]".
    std::string exe;
    if (const char *e = getenv("X86CODE_A64_EXE")) exe = e;
    else if (argc > 1) { exe = argv[1]; argv++; argc--; }
    else { fprintf(stderr, "usage: arm64host <arm64 iOS-simulator executable> [args...]\n"); return 2; }
    // mldr hands over the Linux path; the guest (CFBundleGetMainBundle, _NSGetExecutablePath) needs the Darwin one
    if (!exe.empty() && exe[0] == '/' && exe.compare(0, 20, "/Volumes/SystemRoot/")) {
        int fd = d_open(exe.c_str(), O_RDONLY);
        if (fd >= 0) close(fd);
        else exe = "/Volumes/SystemRoot" + exe;
    }

    // Guest environment: mldr renamed DYLD_* to X86CODE_A64_DYLD_* so Darling's dyld ignores them.
    std::vector<char *> genv;
    std::string root, cache_dir;
    for (char **e = envp; *e; e++) {
        std::string s = *e;
        if (!s.compare(0, 17, "X86CODE_A64_DYLD_")) s = s.substr(12);
        else if (!s.compare(0, 16, "X86CODE_A64_EXE=") || !s.compare(0, 16, "X86CODE_A64_JIT=")) continue;
        if (!s.compare(0, 15, "DYLD_ROOT_PATH=")) root = s.substr(15);
        if (!s.compare(0, 22, "DYLD_SHARED_CACHE_DIR=")) cache_dir = s.substr(22);
        genv.push_back(strdup(s.c_str()));
    }
    if (root.empty()) die("DYLD_ROOT_PATH is not set (the arm64 simulator runtime root)");
    if (cache_dir.empty()) {
        cache_dir = root + "/System/Library/Caches/com.apple.dyld/";
        genv.push_back(strdup(("DYLD_SHARED_CACHE_DIR=" + cache_dir).c_str()));
    }
    genv.push_back(nullptr);

    // Stub dylibs for the host libraries, from the cache placeholders' export tries.
    std::string cache = cache_dir + "/dyld_sim_shared_cache_arm64";
    for (const char *lib : {"kernel", "platform", "pthread"}) {
        std::string path = std::string("/usr/lib/system/libsystem_") + lib + ".dylib";
        build_stub_dylib(path, cache_exports(cache, path));
    }
    native_malloc_prepare(cache);
    read_const_mappings(cache);
    double t_stubs = ms_since(g_t0);

    // Keep the cache's fixed range free for dyld_sim's MAP_FIXED mappings.
    mmap((void *)0x1C0000000ull, 0x2C0000000ull - 0x1C0000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    map_commpage();
    Mapped main_exe = map_macho(exe, false);
    Mapped dyld = map_macho(root + "/usr/lib/dyld_sim", true);
    if (!dyld.entry) die("dyld_sim has no entry point");
    u64 helpers = build_syscall_helpers();

    // KernelArgs as dyld_sim expects them below argv: mainExecutable, argc, argv..., 0, envp..., 0, apple..., 0.
    std::vector<u64> ka;
    ka.push_back(main_exe.mh);
    ka.push_back((u64)argc);
    for (int i = 0; i < argc; i++) ka.push_back((u64)argv[i]);
    ka.push_back(0);
    size_t env_at = ka.size();
    for (char *e : genv) if (e) ka.push_back((u64)e);
    ka.push_back(0);
    size_t apple_at = ka.size();
    ka.push_back((u64)strdup(("executable_path=" + exe).c_str()));
    char buf[128];
    snprintf(buf, sizeof buf, "stack_guard=0x%lx", (unsigned long)((u64)random() << 32 | random()) & ~0xffull); ka.push_back((u64)strdup(buf));
    snprintf(buf, sizeof buf, "malloc_entropy=0x%lx,0x%lx", (unsigned long)random(), (unsigned long)random()); ka.push_back((u64)strdup(buf));
    ka.push_back((u64)strdup("ptr_munge=0x0"));
    ka.push_back(0);
    auto *kargs = new u64[ka.size()];
    memcpy(kargs, ka.data(), ka.size() * 8);
    u64 gargv = (u64)&kargs[2], genvp = (u64)&kargs[env_at], gapple = (u64)&kargs[apple_at];


    GuestThread &t = GuestThread::self();
    install_fault_report();
    double t_setup = ms_since(g_t0);
    u64 glue = 0;
    u64 args[9] = {(u64)argc, gargv, genvp, gapple, main_exe.mh, dyld.mh, dyld.slide, helpers, (u64)&glue};
    u64 main_fn = t.call(dyld.entry, args, 9).rax;
    if (getenv("X86CODE_A64_STATS"))
        logf("stubs %.1f ms, setup %.1f ms, dyld_sim prepare done at %.1f ms; thunk calls %lu", t_stubs, t_setup, ms_since(g_t0),
             (unsigned long)t.svc_count);
    if (!main_fn) die("_dyld_sim_prepare returned no main");
    return (int)t.call(main_fn, {(u64)argc, gargv, genvp, gapple});
}
