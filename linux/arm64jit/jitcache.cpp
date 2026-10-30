// libarm64jit: persistent translation cache for the dyld shared cache's code.
//
// Saves x86 blocks whose guest PC is in the sim shared-cache range to a file under
// X86CODE_A64_JITCACHE_DIR (default $X86SIM_SCRATCH/jitcache), keyed by the dyld cache UUID
// and a build id (bump when emit / StackLayout / fuse_svc / inline objc change). Blocks that
// emit HostCall (svc thunks, CNTPCT, …) are excluded — they bake ASLR-sensitive host pointers.
//
// Load restores the code slab after an identical prelude; relative jumps to terminal handlers
// and inter-block links stay valid. FastDispatch is cleared and refills on use.
#include "a64.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace a64 {

namespace {

constexpr u64 kCacheLo = 0x1c0000000ull;
constexpr u64 kCacheHi = 0x2c0000000ull;
// Bump when emitted code shape changes (StackLayout, fuse_svc_ret, inline objc, HostCall set).
constexpr u64 kBuildId = 0x00000003ull;

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

}  // namespace

void jitcache_prepare(const std::string &cache_path) {
    g_dir = default_dir();
    // cache_path is often a Darling path (/Volumes/SystemRoot/...); this ELF uses glibc fopen.
    std::string linux_path = cache_path;
    const char *prefix = "/Volumes/SystemRoot";
    if (linux_path.rfind(prefix, 0) == 0)
        linux_path = linux_path.substr(strlen(prefix));
    // Collapse accidental double slashes in the directory join from host.cpp.
    while (linux_path.find("//") != std::string::npos)
        linux_path.replace(linux_path.find("//"), 2, "/");
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
    g_path = g_dir + "/" + hex + "-b" + std::to_string(kBuildId) + ".bin";
    if (getenv("X86CODE_A64_STATS"))
        logf("jitcache: path %s", g_path.c_str());
}

void jitcache_try_load(Dynarmic::A64::Jit::SharedCode &shared) {
    if (!g_uuid_ok) return;
    // Opt-in until relocation table lands (ASLR breaks absolute host pointers in the slab).
    if (!(getenv("X86CODE_A64_JITCACHE") && atoi(getenv("X86CODE_A64_JITCACHE")) == 1)) return;
    struct stat st;
    if (stat(g_path.c_str(), &st) != 0) return;
    size_t n = Dynarmic::A64::Jit::LoadPersistentCache(shared, g_path, g_uuid, kBuildId, kCacheLo, kCacheHi);
    if (n)
        logf("jitcache: loaded %zu blocks from %s (%.1f MB)", n, g_path.c_str(), st.st_size / 1048576.0);
    else if (getenv("X86CODE_A64_STATS"))
        logf("jitcache: load miss or mismatch for %s", g_path.c_str());
}

void jitcache_try_save(Dynarmic::A64::Jit::SharedCode &shared) {
    if (!g_uuid_ok) return;
    if (!(getenv("X86CODE_A64_JITCACHE") && atoi(getenv("X86CODE_A64_JITCACHE")) == 1)) return;
    mkdir(g_dir.c_str(), 0755);
    std::string tmp = g_path + ".tmp";
    size_t bytes = Dynarmic::A64::Jit::SavePersistentCache(shared, tmp, g_uuid, kBuildId, kCacheLo, kCacheHi);
    if (!bytes) {
        if (getenv("X86CODE_A64_STATS")) logf("jitcache: nothing to save");
        return;
    }
    if (rename(tmp.c_str(), g_path.c_str()) != 0) {
        logf("jitcache: rename failed");
        unlink(tmp.c_str());
        return;
    }
    logf("jitcache: saved %.1f MB to %s", bytes / 1048576.0, g_path.c_str());
}

void jitcache_set_shared(const std::shared_ptr<Dynarmic::A64::Jit::SharedCode> &sp) {
    g_shared = sp;
}

void jitcache_save_if_shared() {
    if (auto sp = g_shared.lock())
        jitcache_try_save(*sp);
}

}  // namespace a64
