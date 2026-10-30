// Speed 6: native shortcuts inside dyld_sim (iOS 27.0 24A434), applied after dyld_sim is mapped, before it runs.
//
// JustInTimeLoader::matchesPath(state, path) is how dyld finds an already-loaded image for dlopen and friends: for
// each of the ~900 loaded images it compares the path, then (dylibs) the install name. To get the install name it
// builds a full mach_o::Image (Loader::image -> Image::makeFromDyldMapped, which walks every segment and the
// linkedit) and calls Header::installName (another load-command walk through blocks). Under the JIT that was ~54% of
// SpringBoard's dyld_sim prepare (~19 s). The two calls are replaced by one call to a short arm64 routine that reads
// LC_ID_DYLIB from the loader's mach header; the rest of matchesPath (strlen/memcmp, PseudoDylib) is Apple's code.
// X86CODE_A64_DYLD_NATIVE=0 leaves dyld_sim alone.
#include "a64.h"
#include "macho.h"
#include <cstdlib>
#include <cstring>

namespace a64 {

// Header::installName() for a JustInTimeLoader, in arm64, placed in the zero padding at the end of dyld_sim's __TEXT
// (after __unwind_info). x19 = JustInTimeLoader*, [x19+0x20] = its mach header. Returns x0 = the LC_ID_DYLIB name (or
// ""), x1 = its length; clobbers x2-x7. Guest code rather than an svc: it is called ~14M times in SpringBoard's
// prepare, and an svc leaves the JIT's run loop each time (5 s of the 6 s left).
static const uint32_t kInstallName[] = {
    0xf9401262u,  // ldr  x2, [x19, #0x20]
    0xb9401043u,  // ldr  w3, [x2, #0x10]       ncmds
    0x91008044u,  // add  x4, x2, #0x20         first load command
    0x34000123u,  // cbz  w3, none
    0xb9400085u,  // loop: ldr w5, [x4]         cmd
    0x710034bfu,  // cmp  w5, #0xd              LC_ID_DYLIB
    0x54000120u,  // b.eq found
    0xb9400486u,  // ldr  w6, [x4, #4]          cmdsize
    0x34000086u,  // cbz  w6, none
    0x8b060084u,  // add  x4, x4, x6
    0x71000463u,  // subs w3, w3, #1
    0x54ffff21u,  // b.ne loop
    0x10000160u,  // none: adr x0, empty
    0xd2800001u,  // mov  x1, #0
    0xd65f03c0u,  // ret
    0xb9400886u,  // found: ldr w6, [x4, #8]    name offset
    0x8b060080u,  // add  x0, x4, x6
    0xaa0003e1u,  // mov  x1, x0
    0x38401427u,  // strlen: ldrb w7, [x1], #1
    0x35ffffe7u,  // cbnz w7, strlen
    0xcb000021u,  // sub  x1, x1, x0
    0xd1000421u,  // sub  x1, x1, #1
    0xd65f03c0u,  // ret
    0x00000000u,  // empty: ""
};
constexpr u64 kMatchesPathCall = 0x1f2f4;  // JustInTimeLoader::matchesPath+0x44
constexpr u64 kTextPad = 0x5b000;          // __unwind_info ends at 0x5914c, __TEXT at 0x5c000

void dyld_sim_patches(u64 slide) {
    if (const char *e = getenv("X86CODE_A64_DYLD_NATIVE"); e && atoi(e) == 0) return;
    // bl Loader::image; add x0, sp, #8; bl Header::installName  ->  bl kInstallName; nop; nop
    auto *p = (uint32_t *)(slide + kMatchesPathCall);
    auto *pad = (uint32_t *)(slide + kTextPad);
    bool zero = true;
    for (size_t i = 0; i < sizeof kInstallName / 4; i++) zero &= pad[i] == 0;
    if (p[0] != 0x9400107au || p[1] != 0x910023e0u || p[2] != 0x94006a33u || !zero) {
        logf("dyld_sim matchesPath: code mismatch (%#x %#x %#x, pad %s); not patched", p[0], p[1], p[2], zero ? "ok" : "used");
        return;
    }
    memcpy(pad, kInstallName, sizeof kInstallName);
    p[0] = 0x94000000u | (uint32_t)(((kTextPad - kMatchesPathCall) >> 2) & 0x3ffffff);  // bl
    p[1] = 0xd503201fu;  // nop
    p[2] = 0xd503201fu;  // nop
}

}  // namespace a64
