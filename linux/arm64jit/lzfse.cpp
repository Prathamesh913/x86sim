// libarm64jit Speed 32: native LZFSE decode for libcompression's compression_decode_buffer / lzfse_decode_buffer.
//
// A warm app tap draws the app's SplashBoard launch snapshot (.ktx): ImageIO inflates its LZFSE payload, then libate
// decodes the ASTC blocks (native since Speed 31, astc.cpp). The LZFSE part ran as guest code. Here both buffer entry
// points read as "svc #kLzfseSvc*" (MemoryReadCode, as astc.cpp: guest memory is not written, the AOT builder translates
// the same word, so the libcompression chunk must be rebuilt for processes on the image). An LZFSE decode
// (compression_decode_buffer with COMPRESSION_LZFSE, or lzfse_decode_buffer) runs Apple's reference decoder
// (third_party/lzfse, BSD-3, the source Darling ships) and returns its size. Anything else (other algorithms) runs
// libcompression: the replaced first instruction ("stp xA, xB, [sp, #-N]!") is emulated and the guest continues.
//
// The SplashBoard snapshot itself goes through the stream API (ImageIO IIOReadPlugin::decodeLZFSEIntoBuffer:
// compression_stream_init / _process / _destroy). compression_stream_init(DECODE, COMPRESSION_LZFSE) and _reinit get a
// native stream: its state word is a host object. _process gathers the input; once it ends with LZFSE's end-of-stream
// block ("bvx$") the whole stream is decoded at once, then handed out as dst has room (END when all is out). Other
// streams run libcompression (entry instructions "cmp w2, #0x890" / "ldr x8, [x0, #0x20]" emulated).
//
// X86CODE_A64_LZFSE_NATIVE=0: libcompression's own decode; =verify: decodes natively into a side buffer, runs
// libcompression, and compares the two when the call returns (logs mismatches).
#include "a64.h"
extern "C" {
#include "third_party/lzfse/lzfse.h"
}
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace a64 {

namespace {
u64 g_cdb, g_ldb;  // compression_decode_buffer, lzfse_decode_buffer
u32 g_cdb_word, g_ldb_word;
u64 g_sinit, g_sreinit, g_sproc, g_sdestroy;  // compression_stream_init / _reinit / _process / _destroy
constexpr u32 kCmpW2 = 0x7122405fu;    // cmp w2, #0x890
constexpr u32 kLdrX8X0 = 0xf9401008u;  // ldr x8, [x0, #0x20]
struct NStream { std::vector<uint8_t> in, out; size_t given = 0; bool done = false; };
std::mutex g_ns_mu;
std::map<u64, std::unique_ptr<NStream>> g_ns;  // state word -> native stream
std::atomic<u64> g_streams{0};
constexpr u64 kAlgLzfse = 0x801;
std::atomic<u64> g_native{0}, g_fallback{0}, g_mismatch{0}, g_bytes{0};

int mode() {  // 0 off, 1 native, 2 verify
    static const int m = [] {
        const char *e = getenv("X86CODE_A64_LZFSE_NATIVE");
        if (!e) return 1;
        if (!strcmp(e, "verify")) return 2;
        return atoi(e) == 0 ? 0 : 1;
    }();
    return m;
}

// stp Xt, Xt2, [sp, #imm]! (64-bit, pre-index, base sp)
bool is_stp_pre_sp(u32 w) { return (w & 0xffc003e0u) == 0xa98003e0u; }

void emulate_stp_pre(GuestThread &t, u32 w) {
    const int rt = w & 31, rt2 = (w >> 10) & 31;
    const int64_t imm = (int64_t)((int32_t)(((w >> 15) & 0x7f) << 25) >> 25) * 8;
    const u64 sp = t.sp() + imm;
    auto reg = [&](int r) { return r == 31 ? 0 : t.x(r); };
    GuestThread::wr<u64>(sp, reg(rt));
    GuestThread::wr<u64>(sp + 8, reg(rt2));
    t.jit->SetSP(sp);
}

std::mutex g_pend_mu;
u64 g_pend_dst;
std::vector<uint8_t> g_pend;
void check_pending() {
    std::lock_guard<std::mutex> l(g_pend_mu);
    if (!g_pend_dst) return;
    size_t bad = 0;
    const uint8_t *d = (const uint8_t *)(g_pend_dst & GuestThread::kTbi);
    for (size_t i = 0; i < g_pend.size(); i++) bad += d[i] != g_pend[i];
    logf("lzfse verify: %zu bytes, %zu differ", g_pend.size(), bad);
    if (bad) g_mismatch++;
    g_pend_dst = 0;
}

size_t native_decode(u64 dst, size_t dst_size, u64 src, size_t src_size) {
    thread_local std::vector<uint8_t> scratch(lzfse_decode_scratch_size());
    return lzfse_decode_buffer((uint8_t *)(dst & GuestThread::kTbi), dst_size, (const uint8_t *)(src & GuestThread::kTbi),
                               src_size, scratch.data());
}
}  // namespace

void lzfse_prepare(const std::string &cache_path) {
    std::vector<u64> addrs;
    auto names = cache_exports(cache_path, "/usr/lib/libcompression.dylib", &addrs);
    for (size_t i = 0; i < names.size(); i++) {
        if (addrs[i] == ~0ull) continue;
        if (names[i] == "_compression_decode_buffer") g_cdb = addrs[i];
        else if (names[i] == "_lzfse_decode_buffer") g_ldb = addrs[i];
        else if (names[i] == "_compression_stream_init") g_sinit = addrs[i];
        else if (names[i] == "_compression_stream_reinit") g_sreinit = addrs[i];
        else if (names[i] == "_compression_stream_process") g_sproc = addrs[i];
        else if (names[i] == "_compression_stream_destroy") g_sdestroy = addrs[i];
    }
}

std::optional<u32> lzfse_read_code(u64 a) {
    if (!a || !mode()) return std::nullopt;
    if (a == g_sinit || a == g_sreinit || a == g_sproc || a == g_sdestroy) {
        if (mode() != 1) return std::nullopt;  // verify covers the buffer calls only
        const u32 want = (a == g_sinit || a == g_sreinit) ? kCmpW2 : kLdrX8X0;
        if (GuestThread::rd<u32>(a) != want) return std::nullopt;
        return 0xd4000001u | ((a == g_sinit ? kLzfseSvcInit : a == g_sreinit ? kLzfseSvcReinit : a == g_sproc ? kLzfseSvcProc
                                                                                                        : kLzfseSvcDestroy) << 5);
    }
    if (a != g_cdb && a != g_ldb) return std::nullopt;
    u32 &w = a == g_cdb ? g_cdb_word : g_ldb_word;
    if (!w) w = GuestThread::rd<u32>(a);
    if (!is_stp_pre_sp(w)) return std::nullopt;
    return 0xd4000001u | ((a == g_cdb ? kLzfseSvcA : kLzfseSvcB) << 5);
}

void lzfse_step(GuestThread &t, bool lzfse_entry) {
    // compression_decode_buffer(dst, dst_size, src, src_size, scratch, algorithm); lzfse_decode_buffer: the first five
    const int m = mode();
    const u64 dst = t.x(0), dst_size = t.x(1), src = t.x(2), src_size = t.x(3);
    const bool lzfse = lzfse_entry || (t.x(5) & 0xffffffffu) == kAlgLzfse;
    if (m && lzfse && dst && src && dst_size && src_size) {
        if (m == 1) {
            const size_t n = native_decode(dst, dst_size, src, src_size);
            g_bytes += n;
            if (g_native++ % 64 == 0)
                logf("lzfse: native decode %lu -> %lu bytes (%lu so far, %lu MB out)", (unsigned long)src_size,
                     (unsigned long)n, (unsigned long)g_native.load(), (unsigned long)(g_bytes.load() >> 20));
            t.set_x(0, n);
            t.jit->SetPC(t.x(30));
            return;
        }
        // verify: native into a side buffer now; libcompression writes dst, compared at the next call (as astc.cpp)
        std::vector<uint8_t> side(dst_size);
        thread_local std::vector<uint8_t> scratch(lzfse_decode_scratch_size());
        lzfse_decode_buffer(side.data(), dst_size, (const uint8_t *)(src & GuestThread::kTbi), src_size, scratch.data());
        check_pending();
        {
            std::lock_guard<std::mutex> l(g_pend_mu);
            g_pend_dst = dst;
            g_pend = std::move(side);
        }
        emulate_stp_pre(t, lzfse_entry ? g_ldb_word : g_cdb_word);
        return;
    }
    if (g_fallback++ % 256 == 0) logf("lzfse: libcompression decode (algorithm %#lx), %lu so far",
                                      (unsigned long)(t.x(5) & 0xffffffffu), (unsigned long)g_fallback.load());
    emulate_stp_pre(t, lzfse_entry ? g_ldb_word : g_cdb_word);
}

namespace {
void emu_cmp_w2(GuestThread &t) {  // cmp w2, #0x890
    const u32 a = (u32)t.x(2), b = 0x890, r = a - b;
    const u32 n = r >> 31, z = r == 0, c = a >= b, v = ((a ^ b) & (a ^ r)) >> 31;
    t.jit->SetPstate((t.jit->GetPstate() & 0x0fffffffu) | n << 31 | z << 30 | c << 29 | v << 28);
}
void emu_ldr_x8(GuestThread &t) { t.set_x(8, GuestThread::rd<u64>(t.x(0) + 0x20)); }
NStream *find_ns(u64 state) {
    std::lock_guard<std::mutex> l(g_ns_mu);
    auto it = g_ns.find(state);
    return it == g_ns.end() ? nullptr : it->second.get();
}
void drop_ns(u64 state) {
    std::lock_guard<std::mutex> l(g_ns_mu);
    g_ns.erase(state);
}
void ret(GuestThread &t, u64 v) { t.set_x(0, v); t.jit->SetPC(t.x(30)); }
constexpr u64 kOk = 0, kEnd = 1, kErr = (u64)-1;

// whole stream in: decode it (output size unknown up front: grow until it fits)
bool decode_all(NStream &n) {
    thread_local std::vector<uint8_t> scratch(lzfse_decode_scratch_size());
    size_t cap = std::max<size_t>(n.in.size() * 4, 1 << 20);
    for (int i = 0; i < 16; i++, cap *= 2) {
        n.out.resize(cap);
        const size_t r = lzfse_decode_buffer(n.out.data(), cap, n.in.data(), n.in.size(), scratch.data());
        if (r == 0 && !n.in.empty()) return false;
        if (r < cap) { n.out.resize(r); n.done = true; return true; }
    }
    return false;
}
}  // namespace

void lzfse_stream_step(GuestThread &t, u32 swi) {
    const u64 strm = t.x(0) & GuestThread::kTbi;
    if (swi == kLzfseSvcInit || swi == kLzfseSvcReinit) {
        // (stream, operation, algorithm): decode (1) of LZFSE natively
        if ((t.x(1) & 0xffffffffu) == 1 && (t.x(2) & 0xffffffffu) == kAlgLzfse) {
            if (swi == kLzfseSvcReinit) {
                const u64 old = GuestThread::rd<u64>(strm + 0x20);
                if (NStream *n = find_ns(old)) { *n = NStream(); ret(t, kOk); return; }
                // a libcompression stream re-inited as LZFSE decode: let libcompression keep it
                emu_cmp_w2(t);
                return;
            }
            auto n = std::make_unique<NStream>();
            const u64 key = (u64)n.get();
            { std::lock_guard<std::mutex> l(g_ns_mu); g_ns[key] = std::move(n); }
            GuestThread::wr<u64>(strm + 0x20, key);
            if (g_streams++ % 64 == 0) logf("lzfse: native decode stream (%lu so far)", (unsigned long)g_streams.load());
            ret(t, kOk);
            return;
        }
        if (swi == kLzfseSvcReinit) {  // reinit of a native stream to something else: hand it back to libcompression
            const u64 old = GuestThread::rd<u64>(strm + 0x20);
            if (find_ns(old)) {
                drop_ns(old);
                GuestThread::wr<u64>(strm + 0x20, 0);
                // libcompression's reinit of a stream without state behaves as init
                t.jit->SetPC(g_sinit);
                return;
            }
        }
        emu_cmp_w2(t);
        return;
    }
    const u64 state = GuestThread::rd<u64>(strm + 0x20);
    NStream *n = state ? find_ns(state) : nullptr;
    if (!n) { emu_ldr_x8(t); return; }
    if (swi == kLzfseSvcDestroy) {
        drop_ns(state);
        GuestThread::wr<u64>(strm + 0x20, 0);
        ret(t, kOk);
        return;
    }
    // process(stream, flags)
    u64 dst = GuestThread::rd<u64>(strm), dst_size = GuestThread::rd<u64>(strm + 8);
    u64 src = GuestThread::rd<u64>(strm + 0x10), src_size = GuestThread::rd<u64>(strm + 0x18);
    if (!n->done) {
        if (src_size) {
            const uint8_t *p = (const uint8_t *)(src & GuestThread::kTbi);
            n->in.insert(n->in.end(), p, p + src_size);
            GuestThread::wr<u64>(strm + 0x10, src + src_size);
            GuestThread::wr<u64>(strm + 0x18, 0);
        }
        const size_t k = n->in.size();
        const bool eos = k >= 4 && !memcmp(&n->in[k - 4], "bvx$", 4);
        if (!eos) {
            ret(t, (t.x(1) & 1) ? kErr : kOk);  // FINALIZE without the end-of-stream block: truncated
            return;
        }
        if (!decode_all(*n)) { logf("lzfse: native stream decode failed (%zu bytes in)", k); ret(t, kErr); return; }
        g_bytes += n->out.size();
    }
    const size_t left = n->out.size() - n->given, m = std::min<size_t>(left, dst_size);
    if (m) {
        memcpy((void *)(dst & GuestThread::kTbi), n->out.data() + n->given, m);
        n->given += m;
        GuestThread::wr<u64>(strm, dst + m);
        GuestThread::wr<u64>(strm + 8, dst_size - m);
    }
    ret(t, n->given == n->out.size() ? kEnd : kOk);
}

}  // namespace a64
