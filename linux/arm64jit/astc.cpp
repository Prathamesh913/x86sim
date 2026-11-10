// libarm64jit Speed 31: native ASTC decode for libate's at_encoder_decompress_texels.
//
// A warm app tap draws the app's SplashBoard launch snapshot (.ktx: LZFSE + ASTC). ImageIO's ATX reader decodes the
// ASTC blocks through libate (at_encoder_decompress_texels -> ASTCEncoder::DecodeRow per row), ~25-30 ms of guest
// code on SpringBoard's main thread per tap (Speed 22/23). Here the function's first instruction reads as
// "svc #kAstcSvc" (MemoryReadCode, like swiftconf: guest memory is not written, the AOT builder translates the same
// word). The handler asks the encoder for its formats through libate's own getters (guest calls: virtual methods at
// encoder+80), and for LDR ASTC blocks -> RGBA8/BGRA8 texels with the same alpha type on both sides (no conversion
// in libate) it decodes with ARM's astc-encoder (third_party/astcenc) and returns at_error_success. Anything else
// (HDR, BC, 16-bit or float texels, the sRGB->linear flag, 3D) runs libate's own code: the replaced
// "sub sp, sp, #176" is emulated and the guest continues at the next instruction.
//
// Default on (3 clean gates, Speed 31); X86SIM_A64_ASTC_NATIVE=0: libate's own decode; =verify decodes natively into a side
// buffer, lets libate decode, and compares the two at the next call (logs mismatching bytes).
#include "a64.h"
#include "third_party/astcenc/astcenc.h"
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace a64 {

namespace {
u64 g_entry, g_get_texel_format, g_get_block_format, g_get_texel_alpha, g_get_block_alpha;
constexpr u32 kEntryWord = 0xd102c3ffu;  // sub sp, sp, #176

int mode() {  // 0 off, 1 native, 2 verify
    static const int m = [] {
        const char *e = getenv("X86SIM_A64_ASTC_NATIVE");
        if (!e) return 1;
        if (!strcmp(e, "verify")) return 2;
        return atoi(e) == 0 ? 0 : 1;
    }();
    return m;
}

struct Ctx {
    astcenc_context *c = nullptr;
    ~Ctx() { if (c) astcenc_context_free(c); }
};
// per thread and block size (astcenc contexts take one thread index; SpringBoard decodes on several threads)
astcenc_context *ctx_for(unsigned bx, unsigned by) {
    thread_local std::map<u32, std::unique_ptr<Ctx>> m;
    auto &p = m[bx << 8 | by];
    if (!p) {
        p = std::make_unique<Ctx>();
        astcenc_config cfg;
        if (astcenc_config_init(ASTCENC_PRF_LDR, bx, by, 1, ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg) != ASTCENC_SUCCESS ||
            astcenc_context_alloc(&cfg, 1, &p->c) != ASTCENC_SUCCESS) {
            p->c = nullptr;
            logf("astc: no astcenc context for %ux%u", bx, by);
        }
    }
    return p->c;
}

// at_block_format_t 1..14: LDR ASTC footprints
bool ldr_dims(u64 f, unsigned *bx, unsigned *by) {
    static const unsigned d[15][2] = {{0, 0}, {4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8},
                                      {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    if (f < 1 || f > 14) return false;
    *bx = d[f][0];
    *by = d[f][1];
    return true;
}

struct Pending { u64 dst = 0; size_t row = 0, w = 0, h = 0; std::vector<uint8_t> want; };
std::mutex g_pend_mu;
Pending g_pend;
std::atomic<u64> g_native{0}, g_fallback{0}, g_mismatch{0};

void check_pending() {
    std::lock_guard<std::mutex> l(g_pend_mu);
    if (!g_pend.dst) return;
    size_t bad = 0;
    for (size_t y = 0; y < g_pend.h; y++)
        for (size_t i = 0; i < g_pend.w * 4; i++)
            if (std::abs(((const uint8_t *)g_pend.dst)[y * g_pend.row + i] - g_pend.want[y * g_pend.w * 4 + i]) > 1) bad++;
    logf("astc verify: %zux%zu, %zu of %zu bytes differ by more than 1", g_pend.w, g_pend.h, bad, g_pend.w * g_pend.h * 4);
    if (bad) g_mismatch++;
    g_pend.dst = 0;
}

// Decodes src (at_block_buffer_t) into rgba (w*h*4, rows top to bottom). False: not handled here.
bool decode(GuestThread &t, u64 enc, u64 src, u64 dst, u64 flags, std::vector<uint8_t> &rgba, u64 *texel_fmt) {
    if (flags & (1u << 5)) return false;  // sRGB->linear on decode
    const u64 tf = t.call(g_get_texel_format, {enc}) & 0xffffffffu;
    const u64 bf = t.call(g_get_block_format, {enc}) & 0xffffffffu;
    const u64 ta = t.call(g_get_texel_alpha, {enc}) & 0xffffffffu;
    const u64 ba = t.call(g_get_block_alpha, {enc}) & 0xffffffffu;
    unsigned bx, by;
    if ((tf != 5 && tf != 6) || ta != ba || !ldr_dims(bf, &bx, &by)) return false;  // rgba8 / bgra8 unorm
    const u64 blocks = GuestThread::rd<u64>(src), brow = GuestThread::rd<u64>(src + 8);
    const u32 w = GuestThread::rd<u32>(dst + 8), h = GuestThread::rd<u32>(dst + 12), z = GuestThread::rd<u32>(dst + 16);
    if (!blocks || !w || !h || z > 1 || w > 16384 || h > 16384) return false;
    const unsigned nbx = (w + bx - 1) / bx, nby = (h + by - 1) / by;
    if (brow < (u64)nbx * 16) return false;
    astcenc_context *c = ctx_for(bx, by);
    if (!c) return false;
    std::vector<uint8_t> packed;
    const uint8_t *data = (const uint8_t *)(blocks & GuestThread::kTbi);
    if (brow != (u64)nbx * 16) {  // astcenc wants the blocks packed
        packed.resize((size_t)nbx * nby * 16);
        for (unsigned r = 0; r < nby; r++) memcpy(&packed[(size_t)r * nbx * 16], data + r * brow, (size_t)nbx * 16);
        data = packed.data();
    }
    rgba.resize((size_t)w * h * 4);
    void *slices[1] = {rgba.data()};
    astcenc_image img{w, h, 1, ASTCENC_TYPE_U8, slices};
    const astcenc_swizzle sw = tf == 6 ? astcenc_swizzle{ASTCENC_SWZ_B, ASTCENC_SWZ_G, ASTCENC_SWZ_R, ASTCENC_SWZ_A}
                                       : astcenc_swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const astcenc_error e = astcenc_decompress_image(c, data, (size_t)nbx * nby * 16, &img, &sw, 0);
    astcenc_decompress_reset(c);
    if (e != ASTCENC_SUCCESS) {
        logf("astc: astcenc_decompress_image %s", astcenc_get_error_string(e));
        return false;
    }
    *texel_fmt = tf;
    return true;
}
}  // namespace

void astc_prepare(const std::string &cache_path) {
    std::vector<u64> addrs;
    auto names = cache_exports(cache_path, "/usr/lib/libate.dylib", &addrs);
    for (size_t i = 0; i < names.size(); i++) {
        if (addrs[i] == ~0ull) continue;
        if (names[i] == "_at_encoder_decompress_texels") g_entry = addrs[i];
        else if (names[i] == "_at_encoder_get_texel_format") g_get_texel_format = addrs[i];
        else if (names[i] == "_at_encoder_get_block_format") g_get_block_format = addrs[i];
        else if (names[i] == "_at_encoder_get_texel_alpha") g_get_texel_alpha = addrs[i];
        else if (names[i] == "_at_encoder_get_block_alpha") g_get_block_alpha = addrs[i];
    }
    if (!g_get_texel_format || !g_get_block_format || !g_get_texel_alpha || !g_get_block_alpha) g_entry = 0;
}

std::optional<u32> astc_read_code(u64 a) {
    if (a != g_entry || !a) return std::nullopt;
    static const bool ok = GuestThread::rd<u32>(g_entry) == kEntryWord;
    if (!ok) return std::nullopt;
    return 0xd4000001u | (kAstcSvc << 5);
}

void astc_step(GuestThread &t) {
    // x0 encoder, x1 const at_block_buffer_t *src, x2 const at_texel_region_t *dst, x3 at_flags_t
    const int m = mode();
    if (m == 2) check_pending();
    if (m) {
        const u64 enc = t.x(0), src = t.x(1), dst = t.x(2), flags = t.x(3);
        std::vector<uint8_t> rgba;
        u64 tf = 0;
        if (decode(t, enc, src, dst, flags, rgba, &tf)) {
            const u32 w = GuestThread::rd<u32>(dst + 8), h = GuestThread::rd<u32>(dst + 12);
            const u64 out = GuestThread::rd<u64>(dst) & GuestThread::kTbi, row = GuestThread::rd<u64>(dst + 24);
            if (m == 1) {
                const bool flip = flags & (1u << 4);  // texel region bottom to top
                for (u32 y = 0; y < h; y++)
                    memcpy((void *)(out + (u64)(flip ? h - 1 - y : y) * row), &rgba[(size_t)y * w * 4], (size_t)w * 4);
                if (g_native++ % 64 == 0) logf("astc: native decode %ux%u (%lu so far)", w, h, (unsigned long)g_native.load());
                t.set_x(0, 0);  // at_error_success
                t.jit->SetPC(t.x(30));
                return;
            }
            if (!(flags & (1u << 4))) {  // verify (unflipped only): compare after libate wrote it
                std::lock_guard<std::mutex> l(g_pend_mu);
                g_pend = {out, row, w, h, std::move(rgba)};
            }
        } else if (g_fallback++ % 64 == 0) {
            logf("astc: libate decode (format not handled natively), %lu so far", (unsigned long)g_fallback.load());
        }
    }
    // libate's own code: sub sp, sp, #176, then the next instruction
    t.jit->SetSP(t.sp() - 176);
}

}  // namespace a64
