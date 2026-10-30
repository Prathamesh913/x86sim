// Speed 6: native shortcuts inside dyld_sim (iOS 27.0 24A434), applied after dyld_sim is mapped, before it runs.
//
// JustInTimeLoader::matchesPath(state, path) is how dyld finds an already-loaded image for dlopen and friends: for
// each of the ~900 loaded images it compares the path, then (dylibs) the install name. To get the install name it
// builds a full mach_o::Image (Loader::image -> Image::makeFromDyldMapped, which walks every segment and the
// linkedit) and calls Header::installName (another load-command walk through blocks). Under the JIT that was ~54% of
// SpringBoard's dyld_sim prepare (~19 s). The two calls are replaced by one svc that reads LC_ID_DYLIB from the
// loader's mach header natively; the rest of matchesPath (strlen/memcmp, PseudoDylib) is Apple's code.
// X86CODE_A64_DYLD_NATIVE=0 leaves dyld_sim alone.
#include "a64.h"
#include "macho.h"
#include <cstring>

namespace a64 {

static const char g_empty[1] = "";

// x19 = JustInTimeLoader*; [x19+0x20] = its mach_header. Returns Header::installName()'s {x0 = name, x1 = length}.
static void h_jit_loader_install_name(GuestThread &t, u32) {
    const auto *mh = *(const mach_header_64 *const *)(t.x(19) + 0x20);
    const char *name = g_empty;
    size_t len = 0;
    const auto *lc = (const load_command *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const load_command *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_ID_DYLIB) {
            name = (const char *)lc + ((const dylib_command *)lc)->dylib.name_offset;
            len = strlen(name);
            break;
        }
        if (!lc->cmdsize) break;
    }
    t.set_x(0, (u64)name);
    t.set_x(1, len);
}

void dyld_sim_patches(u64 slide) {
    if (const char *e = getenv("X86CODE_A64_DYLD_NATIVE"); e && atoi(e) == 0) return;
    // matchesPath+0x44: bl Loader::image; add x0, sp, #8; bl Header::installName
    auto *p = (uint32_t *)(slide + 0x1f2f4);
    if (p[0] != 0x9400107au || p[1] != 0x910023e0u || p[2] != 0x94006a33u) {
        logf("dyld_sim matchesPath: code mismatch (%#x %#x %#x); not patched", p[0], p[1], p[2]);
        return;
    }
    const u32 idx = add_thunk("dyld_sim JustInTimeLoader install name", h_jit_loader_install_name);
    p[0] = 0xd4000001u | idx << 5;  // svc #idx
    p[1] = 0xd503201fu;             // nop
    p[2] = 0xd503201fu;             // nop
}

}  // namespace a64
