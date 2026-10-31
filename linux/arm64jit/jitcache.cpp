// libarm64jit: persistent translation cache for the dyld shared cache's code.
//
// Two kinds of file in the cache dir, both keyed <uuid>-<build> (build: kBuildId, code-shaping flags and a hash of
// libarm64jit.so's ELF build-id, so any rebuild refuses old files):
//   <key>.<exe>.bin  per process (warm start / AOT training), written on SIGUSR1 (memlog) or at exit with STATS.
//   <key>.aot        Stage 4 AOT image: the union of every .bin's blocks, translated by one builder process
//                    (X86CODE_A64_AOT_BUILD=1, at its exit). Every ASLR-off process (Speed 7; otherwise .app/
//                    executables, or X86CODE_A64_AOT_PROCS=name,name|all) loads it at start instead of its .bin.
//                    X86CODE_A64_AOT=0 disables.
// When the process runs with ADDR_NO_RANDOMIZE (setarch -R), the key also holds libarm64jit's load base: a file
// is used only when host addresses match too (otherwise the reloc table re-bases the emitted host addresses).
//
// Saves x86 blocks whose guest PC is in the sim shared-cache range to a file under
// X86CODE_A64_JITCACHE_DIR (default $X86SIM_SCRATCH/jitcache), keyed by the dyld cache UUID
// and a build id (bump when emit / StackLayout / fuse_svc / inline objc change). Blocks that
// emit HostCall (svc thunks, CNTPCT, …) are excluded — they bake ASLR-sensitive host pointers.
//
// Load restores the code slab after an identical prelude; emitter-recorded relocations fix
// absolute host Imm64s (objc counters, RSB patch sites, …). Relative jumps to terminal handlers
// stay valid when prelude_size matches. FastDispatch is cleared and refills on use.
#include "a64.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <set>
#include <sys/personality.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <thread>

namespace a64 {

namespace {

constexpr u64 kCacheLo = 0x1c0000000ull;
constexpr u64 kCacheHi = 0x2c0000000ull;
// Bump when emitted code shape / cache format changes (StackLayout, fuse_svc, inline objc, reloc table).
constexpr u64 kBuildId = 0x00000013ull;  // Speed 6: compact metadata, mapped AOT image
// kBuildId (bits 0-15), code-shaping env flags (16-18), libarm64jit build-id hash (32-63). The load base is no longer
// part of it (relocations cover every base; a mapped image is keyed by its layout instead, see LoadPersistentCache).
u64 g_build = kBuildId;
std::string g_key, g_aot_path;
bool g_aot_eligible = false;
bool g_aot_loaded = false;

uint8_t g_uuid[16]{};
bool g_uuid_ok = false;
std::string g_dir;
std::string g_path;
std::weak_ptr<Dynarmic::A64::Jit::SharedCode> g_shared;

std::string default_dir() {
    if (const char *d = getenv("X86CODE_A64_JITCACHE_DIR")) return d;
    if (const char *s = getenv("X86SIM_SCRATCH")) return std::string(s) + "/jitcache";
    return "/home/bobby/scratch/x86code/jitcache";
}

// FNV-1a over libarm64jit.so's NT_GNU_BUILD_ID note (and, with ADDR_NO_RANDOMIZE, its load base).
u32 self_hash() {
    struct Ctx { u64 self; u32 h; bool found; } c{(u64)(uintptr_t)&self_hash, 2166136261u, false};
    dl_iterate_phdr([](dl_phdr_info *info, size_t, void *p) -> int {
        auto &c = *(Ctx *)p;
        bool mine = false;
        for (int i = 0; i < info->dlpi_phnum; i++) {
            const auto &ph = info->dlpi_phdr[i];
            u64 lo = info->dlpi_addr + ph.p_vaddr;
            if (ph.p_type == PT_LOAD && c.self >= lo && c.self < lo + ph.p_memsz) mine = true;
        }
        if (!mine) return 0;
        auto mix = [&](const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) c.h = (c.h ^ b[i]) * 16777619u; };
        for (int i = 0; i < info->dlpi_phnum; i++) {
            const auto &ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_NOTE) continue;
            auto *n = (const uint8_t *)(info->dlpi_addr + ph.p_vaddr), *end = n + ph.p_memsz;
            while (n + sizeof(ElfW(Nhdr)) <= end) {
                auto *nh = (const ElfW(Nhdr) *)n;
                const uint8_t *desc = n + sizeof *nh + ((nh->n_namesz + 3) & ~3u);
                if (nh->n_type == NT_GNU_BUILD_ID) { mix(desc, nh->n_descsz); c.found = true; }
                n = desc + ((nh->n_descsz + 3) & ~3u);
            }
        }
        return 1;
    }, &c);
    return c.found ? c.h : 0;
}

// Speed 14: placement.txt lines "addr slot fsize mtime key" (hex addr/slot; key = path from RuntimeRoot/ on, or the
// app bundle path from Containers/ on). The table's hash is part of the key: another layout drops the image.
std::vector<Placement> g_place;
std::string g_runtime_root;  // Linux path of RuntimeRoot/ (from the cache path)
std::vector<bool> g_place_used;
std::mutex g_place_mu;
u32 placement_load(const std::string &dir) {
    if (const char *e = getenv("X86CODE_A64_PLACEMENT"); e && atoi(e) == 0) return 0;
    FILE *f = fopen((dir + "/placement.txt").c_str(), "r");
    if (!f) return 0;
    u32 h = 2166136261u;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        for (char *c = line; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
        Placement p{};
        int n = 0;
        if (sscanf(line, "%lx %lx %lu %lu %n", &p.addr, &p.slot, &p.fsize, &p.mtime, &n) != 4 || !n) continue;
        p.key = line + n;
        while (!p.key.empty() && (p.key.back() == '\n' || p.key.back() == '\r')) p.key.pop_back();
        // inside the cache window's free top only (reserved PROT_NONE by host.cpp; nothing else maps there)
        if (p.addr < 0x2ac000000ull || p.addr + p.slot > kCacheHi || (p.addr & 0x3fff) || p.key.empty()) continue;
        g_place.push_back(p);
    }
    fclose(f);
    g_place_used.assign(g_place.size(), false);
    return g_place.empty() ? 0 : h;
}

bool aot_eligible(const std::string &exe) {
    if (const char *a = getenv("X86CODE_A64_AOT"); a && atoi(a) == 0) return false;
    const char *b = strrchr(exe.c_str(), '/');
    std::string base = b ? b + 1 : exe;
    if (const char *l = getenv("X86CODE_A64_AOT_PROCS")) {
        std::string list = std::string(",") + l + ",";
        // Speed 7: "all" only where the image can be mapped shared (ASLR off: launchd_sim's children); elsewhere it
        // would be a private ~200 MB copy.
        if (list == ",all,") return (personality(0xffffffff) & ADDR_NO_RANDOMIZE) != 0;
        return list.find("," + base + ",") != std::string::npos;
    }
    // Speed 9: metalhostd is back on the image. Its Speed 8 NULL reads (MTLSimImplementation+0x218c/+0x1f6c) were not
    // the image: the logs show them in runs without it too. A command on a connection whose init found no
    // application context (NULL MTLSimApplicationContext*); host/metalhostd now cancels such connections.
    // Speed 9: not launchd_sim. Its libarm64jit lands elsewhere, so its prelude matches the image's only by chance
    // (near/far host calls); when it did, it built a layout of its own: a private 234 MB .map file, +60 MB PSS and
    // a 3 s slower start for the process every other one waits on.
    if (base == "launchd_sim") return false;
    // Speed 7: every process that can map the image shared (ASLR off: launchd_sim's children). With lazy metadata a
    // daemon pays ~1 MB for it and its shared-cache code becomes shared pages. Elsewhere: apps and backboardd.
    if (personality(0xffffffff) & ADDR_NO_RANDOMIZE) return true;
    return exe.find(".app/") != std::string::npos || (b && !strcmp(b + 1, "backboardd"));
}

}  // namespace

const Placement *placement_find(const char *path) {
    const size_t n = strlen(path);
    for (const auto &p : g_place)
        if (n >= p.key.size() && !memcmp(path + n - p.key.size(), p.key.data(), p.key.size()) &&
            (n == p.key.size() || path[n - p.key.size() - 1] == '/'))
            return &p;
    return nullptr;
}

bool placement_claim(const Placement *p, int fd) {
    struct stat st;
    if (fstat(fd, &st) || (u64)st.st_size != p->fsize || (u64)st.st_mtime != p->mtime) return false;
    std::lock_guard<std::mutex> l(g_place_mu);
    const size_t i = p - g_place.data();
    if (g_place_used[i]) return false;
    g_place_used[i] = true;
    return true;
}

void jitcache_prepare(const std::string &cache_path, const std::string &exe) {
    // Speed 7: FastDispatch table per process: 256K entries (4 MB) for apps and backboardd, whose hot sets need it;
    // 64K (1 MB) for daemons. X86CODE_A64_FD_ENTRIES=n overrides.
    {
        const char *b = strrchr(exe.c_str(), '/');
        size_t n = exe.find(".app/") != std::string::npos || (b && !strcmp(b + 1, "backboardd")) ? 0x40000 : 0x10000;
        if (const char *e = getenv("X86CODE_A64_FD_ENTRIES")) n = strtoul(e, nullptr, 0);
        Dynarmic::A64::Jit::SetFastDispatchEntries(n);
    }
    g_dir = default_dir();
    // cache_path is often a Darling path (/Volumes/SystemRoot/...); this ELF uses glibc fopen.
    std::string linux_path = cache_path;
    const char *prefix = "/Volumes/SystemRoot";
    if (linux_path.rfind(prefix, 0) == 0)
        linux_path = linux_path.substr(strlen(prefix));
    // Collapse accidental double slashes in the directory join from host.cpp.
    while (linux_path.find("//") != std::string::npos)
        linux_path.replace(linux_path.find("//"), 2, "/");
    if (size_t r = linux_path.find("/System/Library/Caches/com.apple.dyld/"); r != std::string::npos)
        g_runtime_root = linux_path.substr(0, r + 1);
    FILE *f = fopen(linux_path.c_str(), "rb");
    if (!f) {
        logf("jitcache: cannot open %s", linux_path.c_str());
        return;
    }
    // dyld_cache_header.uuid at 0x58 on this cache format.
    if (fseek(f, 0x58, SEEK_SET) != 0 || fread(g_uuid, 1, 16, f) != 16) {
        fclose(f);
        logf("jitcache: cannot read UUID");
        return;
    }
    fclose(f);
    g_uuid_ok = true;
    char hex[40];
    for (int i = 0; i < 16; i++) sprintf(hex + 2 * i, "%02x", g_uuid[i]);
    auto on = [](const char *e, bool dflt) { const char *v = getenv(e); return v ? atoi(v) == 1 : dflt; };
    const u32 h = self_hash();
    if (!h) { g_uuid_ok = false; logf("jitcache: no build-id in libarm64jit.so; cache off"); return; }
    g_build = kBuildId | (u64)on("X86CODE_A64_FUSE_SVC", false) << 16 | (u64)on("X86CODE_A64_INLINE_OBJC", true) << 17 |
              (u64)timebase_apple() << 18 |  // CNTFRQ and the Swift deadline patch are in the code
              (u64)h << 32;
    if (u32 ph = placement_load(g_dir)) {
        g_build ^= (u64)ph << 32;
        if (getenv("X86CODE_A64_STATS")) logf("placement: %zu files, table %08x", g_place.size(), ph);
    }
    char bh[24];
    snprintf(bh, sizeof bh, "%016llx", (unsigned long long)g_build);
    g_key = std::string(hex) + "-" + bh;
    const char *b = strrchr(exe.c_str(), '/');
    g_path = g_dir + "/" + g_key + "." + (b ? b + 1 : exe.c_str()) + ".bin";
    g_aot_path = g_dir + "/" + g_key + ".aot";
    g_aot_eligible = aot_eligible(exe);
    if (getenv("X86CODE_A64_STATS"))
        logf("jitcache: path %s", g_path.c_str());
}

void jitcache_try_load(Dynarmic::A64::Jit::SharedCode &shared) {
    if (!g_uuid_ok) return;
    // Default on. X86CODE_A64_JITCACHE=0 disables load/save.
    if (getenv("X86CODE_A64_JITCACHE") && atoi(getenv("X86CODE_A64_JITCACHE")) == 0) return;
    struct stat st;
    const std::string &path = g_aot_eligible && stat(g_aot_path.c_str(), &st) == 0 ? g_aot_path : g_path;
    if (stat(path.c_str(), &st) != 0) return;
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    // Speed 6: the AOT image's code is mapped from a relocated copy shared by every process with the same layout
    // (X86CODE_A64_AOT_SHARE=0: private copy as before).
    const bool share = &path == &g_aot_path && !(getenv("X86CODE_A64_AOT_SHARE") && atoi(getenv("X86CODE_A64_AOT_SHARE")) == 0);
    size_t n = Dynarmic::A64::Jit::LoadPersistentCache(shared, path, g_uuid, g_build, kCacheLo, kCacheHi, share);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_aot_loaded = n && &path == &g_aot_path;
    if (n)
        logf("jitcache: loaded %zu blocks from %s (%.1f MB, %.0f ms)", n, path.c_str(), st.st_size / 1048576.0,
             (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    else
        logf("jitcache: load miss or mismatch for %s", path.c_str());
}

void jitcache_try_save(Dynarmic::A64::Jit::SharedCode &shared, const std::string &to) {
    if (!g_uuid_ok) return;
    const std::string &dst = to.empty() ? g_path : to;
    if (getenv("X86CODE_A64_JITCACHE") && atoi(getenv("X86CODE_A64_JITCACHE")) == 0) return;
    mkdir(g_dir.c_str(), 0755);
    std::string tmp = dst + ".tmp." + std::to_string(getpid());  // Speed 8: two sim instances save the same <exe>.bin
    size_t bytes = Dynarmic::A64::Jit::SavePersistentCache(shared, tmp, g_uuid, g_build, kCacheLo, kCacheHi);
    if (!bytes) {
        if (getenv("X86CODE_A64_STATS")) logf("jitcache: nothing to save");
        return;
    }
    if (rename(tmp.c_str(), dst.c_str()) != 0) {
        logf("jitcache: rename failed");
        unlink(tmp.c_str());
        return;
    }
    logf("jitcache: saved %.1f MB to %s", bytes / 1048576.0, dst.c_str());
}

// Speed 5: the cache used to be written only on SIGUSR1 or at exit with X86CODE_A64_STATS, so normal use never warmed
// it. Now a low-priority thread saves this process's .bin once translation has been quiet (<500 new blocks per 10 s)
// for ~20 s, or every 120 s while it keeps growing, when at least
// kMinNewBlocks blocks were added since the last save/load, and __exit saves under the same threshold
// (jitcache_save_on_exit). The snapshot holds the shared lock only for the copy (no unlink; see SavePersistentCache).
// X86CODE_A64_JITCACHE_AUTOSAVE=0 turns both off.
constexpr u64 kMinNewBlocks = 5000;
std::atomic<u64> g_saved_blocks{0};

// Speed 9: not in a process that loaded the AOT image (X86CODE_A64_AOT_AUTOSAVE=1 to keep it, for training .bins). Its
// .bin is never read back while the image exists, yet every such process wrote one of ~240 MB (the image's blocks
// registered so far): ~6.5 GB per boot.
static bool autosave_on() {
    const char *e = getenv("X86CODE_A64_JITCACHE_AUTOSAVE");
    const char *a = getenv("X86CODE_A64_AOT_AUTOSAVE");
    if (g_aot_loaded && !(a && atoi(a) == 1)) return false;
    return !(e && atoi(e) == 0) && !(getenv("X86CODE_A64_JITCACHE") && atoi(getenv("X86CODE_A64_JITCACHE")) == 0) &&
           !getenv("X86CODE_A64_AOT_BUILD");
}
static void save_if_grown(Dynarmic::A64::Jit::SharedCode &sc, const char *why) {
    if (!autosave_on()) return;  // the image is loaded after the saver thread starts
    const u64 n = Dynarmic::A64::Jit::GetSharedStats(sc).blocks;
    if (n < g_saved_blocks.load() + kMinNewBlocks) return;
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    jitcache_try_save(sc);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_saved_blocks = n;
    logf("jitcache: %s save at %llu blocks (%.0f ms)", why, (unsigned long long)n,
         (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
}

void jitcache_set_shared(const std::shared_ptr<Dynarmic::A64::Jit::SharedCode> &sp) {
    g_shared = sp;
    g_saved_blocks = Dynarmic::A64::Jit::GetSharedStats(*sp).blocks;  // what the load brought in
    if (!g_uuid_ok || !autosave_on()) return;
    // The thread is not a Darling thread: with signals unblocked, a process-directed signal delivered to it made
    // mldr abort (metalhostd SIGABRT on the saver's tid). Block everything here; the guest's threads take them.
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    std::thread([] {
        g_non_darling_thread = true;
        setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
        // Busy processes (SpringBoard, backboardd, Preferences) never stop translating entirely: "quiet" is under
        // 500 new blocks per 10 s, and a save is forced every 120 s while the hot set keeps growing.
        u64 last = 0;
        int quiet = 0, since = 0;
        for (;;) {
            sleep(10);
            auto sp = g_shared.lock();
            if (!sp) return;
            const u64 n = Dynarmic::A64::Jit::GetSharedStats(*sp).blocks;
            quiet = n - last < 500 ? quiet + 1 : 0;
            last = n;
            since++;
            if (quiet >= 2 || since >= 12) {
                const u64 before = g_saved_blocks.load();
                save_if_grown(*sp, quiet >= 2 ? "idle" : "periodic");
                if (g_saved_blocks.load() != before) since = 0;
            }
        }
    }).detach();
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
}

void jitcache_save_on_exit() {
    if (!autosave_on()) return;
    if (auto sp = g_shared.lock()) save_if_grown(*sp, "exit");
}

// X86CODE_A64_AOT_BUILD=1, at the builder's exit: translate the union of every training .bin's blocks (same key)
// on this thread and save them as the AOT image. Stops short of the code cache's end (a full cache is cleared).
// The AOT builder translates blocks of placed files too: map each file's segments (read-only; code needs no fixups)
// at its slot, as dyld_sim would in a sim process. Returns the number mapped.
int placement_map_all() {
    int n = 0;
    for (const auto &p : g_place) {
        if (g_runtime_root.empty() || p.key.rfind("Containers/", 0) == 0) continue;
        const std::string path = g_runtime_root + p.key;
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;
        struct stat st;
        uint8_t hdr[0x10000];
        u64 slice = 0;
        ssize_t got = fstat(fd, &st) == 0 && (u64)st.st_size == p.fsize ? pread(fd, hdr, sizeof hdr, 0) : -1;
        if (got >= 8 && !memcmp(hdr, "\xca\xfe\xba\xbe", 4)) {  // fat: the arm64 slice
            const uint32_t nf = __builtin_bswap32(*(const uint32_t *)(hdr + 4));
            for (uint32_t i = 0; i < nf && 8 + 20 * (i + 1) <= (uint32_t)got; i++)
                if (__builtin_bswap32(*(const u32 *)(hdr + 8 + 20 * i)) == 0x0100000Cu) slice = __builtin_bswap32(*(const u32 *)(hdr + 16 + 20 * i));
            got = slice ? pread(fd, hdr, sizeof hdr, slice) : -1;
        }
        if (got < 32 || *(const u32 *)hdr != 0xfeedfacfu) { close(fd); continue; }
        const u32 ncmds = *(const u32 *)(hdr + 16);
        u64 lo = ~0ull;
        for (int pass = 0; pass < 2; pass++) {
            u32 off = 32;
            for (u32 i = 0; i < ncmds && off + 72 <= (u32)got; i++) {
                const u32 cmd = *(const u32 *)(hdr + off), cs = *(const u32 *)(hdr + off + 4);
                if (cmd == 0x19) {
                    u64 vm, vs, fo, fs; memcpy(&vm, hdr + off + 24, 8); memcpy(&vs, hdr + off + 32, 8);
                    memcpy(&fo, hdr + off + 40, 8); memcpy(&fs, hdr + off + 48, 8);
                    const int ip = *(const int *)(hdr + off + 60);
                    if (!(ip == 0 && fs == 0)) {
                        if (pass == 0) lo = std::min(lo, vm);
                        else if (fs) mmap((void *)(p.addr + vm - lo), fs, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, slice + fo);
                    }
                }
                if (!cs) break;
                off += cs;
            }
        }
        close(fd);
        n++;
    }
    return n;
}

void jitcache_aot_build(GuestThread &t) {
    auto sp = g_shared.lock();
    if (!g_uuid_ok || !sp) { logf("aot: no shared code / cache key"); return; }
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    std::set<u64> descs;
    int files = 0;
    if (DIR *d = opendir(g_dir.c_str())) {
        // Any build's training files for this image: the hot set (block PCs) outlives codegen changes.
        const std::string pre = g_key.substr(0, 32) + "-";
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind(pre, 0) != 0 || n.size() < 4 || n.compare(n.size() - 4, 4, ".bin") != 0) continue;
            auto v = Dynarmic::A64::Jit::ReadPersistentDescriptors(g_dir + "/" + n, g_uuid, 0);
            logf("aot: %s: %zu blocks", n.c_str(), v.size());
            descs.insert(v.begin(), v.end());
            files++;
        }
        closedir(d);
    }
    const u64 limit = (u64)(getenv("X86CODE_A64_CODECACHE_MB") ? atoi(getenv("X86CODE_A64_CODECACHE_MB")) : 256) * 7 / 8 << 20;
    if (!g_place.empty()) logf("aot: mapped %d of %zu placed files", placement_map_all(), g_place.size());
    size_t done = 0;
    for (u64 ld : descs) {
        if (Dynarmic::A64::Jit::GetSharedStats(*sp).code_bytes > limit) { logf("aot: code cache limit reached"); break; }
        t.jit->Pretranslate(ld);
        done++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    logf("aot: %d files, %zu unique blocks, translated %zu in %.1f s", files, descs.size(), done,
         (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    jitcache_try_save(*sp, g_aot_path);
}

void jitcache_save_if_shared() {
    if (auto sp = g_shared.lock()) {
        jitcache_try_save(*sp);
        g_saved_blocks = Dynarmic::A64::Jit::GetSharedStats(*sp).blocks;
    }
}

}  // namespace a64
