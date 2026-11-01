// x86Code Speed 25: native step for the Swift runtime's type lookup by mangled name, conformance-record fallback
// (_searchConformancesByMangledTypeName, libswiftCore, iOS 27.x).
//
// When swift_getTypeByMangledName / _findContextDescriptor misses its cache and the type-metadata records, it walks
// every protocol conformance record of every loaded image (627 sections, ~346K records in Settings on 27.2; 612 of the
// sections are the shared cache's: the cache's dyld Swift tables are used for conformance lookups, not for this walk)
// and calls _contextDescriptorMatchesMangling(record's type descriptor, node) for each record that names a type
// descriptor. Under the JIT that loop was ~2/3 of the guest samples of a cold Settings open on 27.2.
//
// The loop head ("ldrsw x8, [x27]": the next record) is read by the JIT as "svc #kSwiftTypeSvc" (MemoryReadCode, like
// swiftconf; guest memory is not written, so the AOT builder sees the same code). The handler walks the records
// natively, in the same order, resolving the type descriptor exactly as the guest does (TypeReferenceKind 0 direct, 1
// indirect, others skipped), and stops at the next record the matcher has to see, with the registers the guest loop
// has at its "mov x0, x21" (call) or, after the last section, at the not-found exit. A record is passed over natively
// only when the matcher certainly answers false: the node is a nominal type node whose name child is an Identifier,
// the descriptor is a class/struct/enum descriptor without import info (its ABI name is its Name), and the two names
// differ. Everything else still goes to the guest matcher.
// X86CODE_A64_SWIFT_TYPE_NATIVE=0: off. X86CODE_A64_SWIFT_TYPE_FILTER=0: walk natively but call the matcher for every
// descriptor. X86CODE_A64_SWIFT_TYPE_DEBUG=1: counters and the first nodes, logged.
#include "a64.h"

#include <atomic>
#include <cstring>
#include <array>
#include <map>
#include <string>
#include <mutex>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

namespace a64 {
using s32 = int32_t;
using s64 = int64_t;
using u16 = uint16_t;

// ---- libswiftCore's __TEXT in the shared cache (mapped at 0x1c0000000) ---------------------------------------------
// The dyld cache read from its files (the main file and its .01, .02, ... subcaches, each with its own mapping table):
// guest cache pages are mapped on demand (and the image builder maps the header page without access), so these
// lookups never touch guest memory.
namespace {
struct CacheFiles {
    struct Map { u64 addr, size, off; FILE *f; };
    std::vector<Map> maps;
    CacheFiles() {
        const std::string &base = jitcache_cache_file();
        for (int i = 0; i < 64; i++) {
            char suf[8] = "";
            if (i) snprintf(suf, sizeof suf, ".%02d", i);
            FILE *f = fopen((base + suf).c_str(), "rb");
            if (!f) break;
            u32 mo[2];
            if (fseek(f, 0x10, SEEK_SET) || fread(mo, 4, 2, f) != 2) { fclose(f); break; }
            for (u32 m = 0; m < mo[1]; m++) {
                u64 e[3];
                if (fseek(f, (long)(mo[0] + 32ull * m), SEEK_SET) || fread(e, 8, 3, f) != 3) break;
                maps.push_back({e[0], e[1], e[2], f});
            }
        }
    }
    bool read(u64 addr, void *out, size_t n) const {
        for (const Map &m : maps)
            if (addr >= m.addr && addr + n <= m.addr + m.size)
                return fseek(m.f, (long)(m.off + addr - m.addr), SEEK_SET) == 0 && fread(out, 1, n, m.f) == n;
        return false;
    }
    bool read_file0(u64 off, void *out, size_t n) const {
        FILE *f = maps.empty() ? nullptr : maps[0].f;
        return f && fseek(f, (long)off, SEEK_SET) == 0 && fread(out, 1, n, f) == n;
    }
};
std::mutex g_cf_mu;
const CacheFiles &cache_files() { static const CacheFiles cf; return cf; }
}  // namespace

static bool swift_text(u64 &lo, u64 &hi) {
    static u64 s_lo = 0, s_hi = 0;
    static const bool ok = [] {
        std::lock_guard l(g_cf_mu);
        const CacheFiles &cf = cache_files();
        // dyld_cache_header imagesOffset/Count at 0x1c0; 32-byte entries: address, modTime, inode, pathFileOffset
        u32 hdr[2];
        u64 mh = 0;
        if (cf.read_file0(0x1c0, hdr, 8)) {
            static const char kName[] = "/libswiftCore.dylib";
            for (u32 i = 0; i < hdr[1] && !mh; i++) {
                u64 e[4];
                char path[512] = {};
                if (!cf.read_file0(hdr[0] + 32ull * i, e, 32) || !cf.read_file0((u32)e[3], path, sizeof path - 1)) break;
                const size_t n = strlen(path);
                if (n >= sizeof kName - 1 && strcmp(path + n - (sizeof kName - 1), kName) == 0) mh = e[0];
            }
        }
        u32 h[8];
        if (!mh || !cf.read(mh, h, 32) || h[0] != 0xfeedfacfu) {
            logf("swift text: libswiftCore not found in %s (header %#llx)", jitcache_cache_file().c_str(), (unsigned long long)mh);
            return false;
        }
        u64 lc = mh + 32;
        for (u32 c = 0; c < h[4]; c++) {
            uint8_t cmd[48];
            if (!cf.read(lc, cmd, sizeof cmd)) return false;
            u32 k, sz;
            memcpy(&k, cmd, 4);
            memcpy(&sz, cmd + 4, 4);
            if (k == 0x19 && strncmp((const char *)cmd + 8, "__TEXT", 16) == 0) {
                memcpy(&s_lo, cmd + 24, 8);
                u64 vs;
                memcpy(&vs, cmd + 32, 8);
                s_hi = s_lo + vs;
                return true;
            }
            lc += sz;
        }
        return false;
    }();
    lo = s_lo; hi = s_hi;
    return ok;
}

u32 swift_text_word(u64 a) {
    u32 w = 0;
    std::lock_guard l(g_cf_mu);
    cache_files().read(a, &w, 4);
    return w;
}

// First address in libswiftCore's __TEXT where words[i] (under mask[i], default all bits) follow each other, or 0.
u64 swift_text_find(const u32 *words, const u32 *mask, size_t n) {
    u64 lo, hi;
    if (!n || n > 1024 || !swift_text(lo, hi)) return 0;
    std::lock_guard l(g_cf_mu);
    std::vector<u32> t((hi - lo) / 4);
    if (!cache_files().read(lo, t.data(), t.size() * 4)) return 0;
    for (size_t s0 = 0; s0 + n <= t.size(); s0++) {
        size_t i = 0;
        for (; i < n; i++) {
            const u32 m = mask ? mask[i] : ~0u;
            if ((t[s0 + i] & m) != (words[i] & m)) break;
        }
        if (i == n) return lo + 4 * s0;
    }
    return 0;
}

namespace {
// From the loop's "add x23, x23, #0x10" (next section) to its last word (27.2: 0x1c15f2dac .. 0x1c15f2e28); the
// matcher call (bl) is masked.
constexpr u32 kLoop[] = {
    0x910042f7, 0xeb1802ff, 0x540003c0, 0xa9406af9, 0xeb1a033f, 0x54ffff60, 0xaa1903fb, 0x1400000c,
    0xb9800109, 0xf8686935, 0xb40000b5, 0xaa1503e0, 0xaa1403e1, 0x94000000, 0x350005e0, 0x9100137b,
    0x91001339, 0xeb1a037f, 0x54fffdc0, 0xb9800368, 0x8b080329, 0x91001128, 0xb9400d29, 0x53031529,
    0x7100053f, 0x54fffde0, 0x35fffea9, 0xb9800109, 0x34fffe69, 0x8b080135, 0xb5fffdb5, 0x17fffff0,
};
constexpr u32 kLoopMask[] = {
    ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, 0xfc000000u, ~0u, ~0u,
    ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u,
};
u64 kBase = 0;  // the "add x23, x23, #0x10"
constexpr u64 kCallOff = 0x2c, kStepOff = 0x4c, kDoneOff = 0x80;

bool enabled() {
    static const bool ok = [] {
        const char *e = getenv("X86CODE_A64_SWIFT_TYPE_NATIVE");
        if (e && atoi(e) == 0) return false;
        kBase = swift_text_find(kLoop, kLoopMask, sizeof kLoop / 4);
        if (!kBase) { logf("swift type scan step: code not found in libswiftCore; not used"); return false; }
        return true;
    }();
    return ok;
}
const bool g_dbg = getenv("X86CODE_A64_SWIFT_TYPE_DEBUG") != nullptr;
const bool g_index = !(getenv("X86CODE_A64_SWIFT_TYPE_INDEX") && atoi(getenv("X86CODE_A64_SWIFT_TYPE_INDEX")) == 0);
const bool g_filter = !(getenv("X86CODE_A64_SWIFT_TYPE_FILTER") && atoi(getenv("X86CODE_A64_SWIFT_TYPE_FILTER")) == 0);
std::atomic<u64> g_steps{0}, g_recs{0}, g_calls{0}, g_dumps{0};

void dump_node(u64 node) {
    auto q = [](u64 a) { return (unsigned long long)GuestThread::rd<u64>(a); };
    const unsigned pk = GuestThread::rd<uint8_t>(node + 18);
    const u64 c1 = pk == 2 ? GuestThread::rd<u64>(node + 8) : 0;
    logf("swifttype node kind %u payload %u; child1 kind %u payload %u", GuestThread::rd<u16>(node + 16), pk,
         c1 ? GuestThread::rd<u16>(c1 + 16) : 0, c1 ? GuestThread::rd<uint8_t>(c1 + 18) : 0);
    if (pk != 2) return;
    logf("swifttype node %#llx: %016llx %016llx %016llx %016llx", (unsigned long long)node, q(node), q(node + 8), q(node + 16), q(node + 24));
    for (int c = 0; c < 2; c++) {
        const u64 ch = GuestThread::rd<u64>(node + 8 * c);
        if (ch < 0x100000000ull || ch > 0x7fffffffffffull) continue;
        logf("  child%d %#llx: %016llx %016llx %016llx", c, (unsigned long long)ch, q(ch), q(ch + 8), q(ch + 16));
        const u64 t = GuestThread::rd<u64>(ch), len = GuestThread::rd<u64>(ch + 8);
        if (t > 0x100000000ull && t < 0x7fffffffffffull && len > 0 && len < 64) {
            char buf[80];
            memcpy(buf, (const void *)t, len);
            buf[len] = 0;
            for (u64 i = 0; i < len; i++) if (buf[i] < 32 || buf[i] > 126) buf[i] = '?';
            logf("  child%d text \"%s\"", c, buf);
        }
    }
}

// The type descriptor a conformance record names, as the guest loop resolves it (0: none, the loop skips it).
u64 record_type(u64 rec) {
    const u64 desc = rec + (s64)GuestThread::rd<s32>(rec);
    const u32 kind = (GuestThread::rd<u32>(desc + 12) >> 3) & 7;
    const u64 field = desc + 4;
    const s64 rel = GuestThread::rd<s32>(field);
    if (kind == 1) return GuestThread::rd<u64>(field + rel);
    if (kind == 0) return rel ? field + rel : 0;
    return 0;
}

// The name the node certainly compares against (nullptr: unknown, the matcher decides).
struct Want { const char *p = nullptr; u64 n = 0; };
// Demangle::Node (swift/Demangling/Demangle.h): a 16-byte union {StringRef Text; NodePointer InlineChildren[2];
// NodeVector Children; ...} at +0, Kind (u16) at +16, PayloadKind (u8) at +18: TwoChildren 2, Text 3, ManyChildren 5.
// Nominal type nodes (Class, Structure, Enum, ...) have two children: the context and the name.
// Kinds of this runtime's Swift (one more node kind than Swift 6.2 before Class): Identifier 104 (a Text payload;
// 6.2's 104 is Index, an Index payload), Class 26. Structure and Enum are confirmed from the walks in debug mode (the
// descriptor kind of the records that carry the node's name), see kKindStruct/kKindEnum.
constexpr u16 kKindIdent = 104, kKindClass = 26;
u16 g_kind_struct = 0, g_kind_enum = 0;
// debug: node kind -> counts of name-matching descriptor kinds (class, struct, enum)
std::mutex g_hist_mu;
std::map<u16, std::array<u64, 3>> g_hist;
Want node_name(u64 node, bool any_kind = false) {
    if (GuestThread::rd<uint8_t>(node + 18) != 2) return {};
    const u16 k = GuestThread::rd<u16>(node + 16);
    if (!any_kind && k != kKindClass && (!g_kind_struct || k != g_kind_struct) && (!g_kind_enum || k != g_kind_enum)) return {};
    const u64 name = GuestThread::rd<u64>(node + 8);
    if (!name || GuestThread::rd<uint8_t>(name + 18) != 3 || GuestThread::rd<u16>(name + 16) != kKindIdent) return {};
    const u64 len = GuestThread::rd<u64>(name + 8);
    if (!len || len > 4096) return {};
    return {(const char *)GuestThread::rd<u64>(name), len};
}
bool name_equals(u64 td, const Want &w) {
    const u64 nf = td + 8;
    const s64 rel = GuestThread::rd<s32>(nf);
    if (!rel) return false;
    const char *name = (const char *)(nf + rel);
    return strncmp(name, w.p, w.n) == 0 && name[w.n] == 0;
}
// false only when _contextDescriptorMatchesMangling(td, node) is certainly false
bool may_match(u64 td, const Want &w) {
    if (!w.p) return true;
    const u32 flags = GuestThread::rd<u32>(td);
    const u32 kind = flags & 0x1f;
    if (kind < 16 || kind > 18) return true;  // not a class/struct/enum descriptor
    if (flags & (1u << 18)) return true;      // HasImportInfo: the ABI name may differ from Name
    if (!GuestThread::rd<s32>(td + 8)) return true;
    return name_equals(td, w);
}

// ---- the index: walk position -> next record the matcher must see, for a given type name ------------------------
// Positions are (section number << 32 | byte offset in the section). Records whose descriptor is a class/struct/enum
// without import info are keyed by FNV-1a of their Name (sorted (hash, position) pairs, ~16 bytes a record); the
// rest that name a descriptor ("other") always go to the matcher. Sections are only ever appended to the runtime's
// array (images load); a changed prefix (never seen) rebuilds from scratch. One mutex: lookups are short.
struct TypeIndex {
    std::vector<u64> begins, ends;
    std::vector<std::pair<u64, u64>> named;  // (hash, pos), sorted
    std::vector<u64> other;                  // pos, sorted
};
std::mutex g_ix_mu;
TypeIndex g_ix;
std::atomic<u64> g_ix_records{0};
u64 fnv(const char *p, u64 n) {
    u64 h = 0xcbf29ce484222325ull;
    for (u64 i = 0; i < n; i++) h = (h ^ (uint8_t)p[i]) * 0x100000001b3ull;
    return h;
}
// the key of a record's type descriptor, or false if the matcher must judge it
bool descriptor_key(u64 td, u64 &h) {
    const u32 flags = GuestThread::rd<u32>(td);
    const u32 kind = flags & 0x1f;
    if (kind < 16 || kind > 18 || (flags & (1u << 18))) return false;
    const s64 rel = GuestThread::rd<s32>(td + 8);
    if (!rel) return false;
    const char *name = (const char *)(td + 8 + rel);
    h = fnv(name, strlen(name));
    return true;
}
// under g_ix_mu: make the index cover sections [0, count) of the array at base
void index_sync(u64 base, u64 count) {
    TypeIndex &ix = g_ix;
    bool same = ix.begins.size() <= count;
    for (size_t i = 0; same && i < ix.begins.size(); i++)
        same = ix.begins[i] == GuestThread::rd<u64>(base + 16 * i) && ix.ends[i] == GuestThread::rd<u64>(base + 16 * i + 8);
    if (!same) ix = TypeIndex{};
    const size_t from = ix.begins.size(), named0 = ix.named.size(), other0 = ix.other.size();
    if (from == count) return;
    for (u64 i = from; i < count; i++) {
        const u64 b = GuestThread::rd<u64>(base + 16 * i), e = GuestThread::rd<u64>(base + 16 * i + 8);
        ix.begins.push_back(b);
        ix.ends.push_back(e);
        for (u64 r = b; r != e; r += 4) {
            const u64 pos = i << 32 | (r - b);
            // Speed 27: a record that names its type indirectly (kind 1: a pointer slot dyld binds) is resolved at
            // lookup time, as the guest loop does: when the section is indexed right after its image was added the
            // slot may not be bound yet (0), and such records were dropped for good. SwiftUI's conformance for
            // DynamicContainerInfo<DynamicLayoutViewAdaptor> was then never found (an AttributeGraph precondition
            // "attribute failed to set an initial value": Maps aborted building a route card).
            const u64 desc = r + (s64)GuestThread::rd<s32>(r);
            if (((GuestThread::rd<u32>(desc + 12) >> 3) & 7) == 1) { ix.other.push_back(pos); continue; }
            const u64 td = record_type(r);
            if (!td) continue;
            u64 h;
            if (descriptor_key(td, h)) ix.named.emplace_back(h, pos);
            else ix.other.push_back(pos);
        }
        g_ix_records += (e - b) / 4;
    }
    // new positions are larger than all old ones: sort the new part, then merge
    std::sort(ix.named.begin() + named0, ix.named.end());
    std::inplace_merge(ix.named.begin(), ix.named.begin() + named0, ix.named.end());
    // ix.other: appended in position order already
    (void)other0;
}
// under g_ix_mu: the first position >= pos whose record the matcher must see for name w (~0 if none)
u64 index_next(u64 pos, const Want &w) {
    const TypeIndex &ix = g_ix;
    u64 best = ~0ull;
    auto o = std::lower_bound(ix.other.begin(), ix.other.end(), pos);
    if (o != ix.other.end()) best = *o;
    const u64 h = fnv(w.p, w.n);
    for (auto it = std::lower_bound(ix.named.begin(), ix.named.end(), std::make_pair(h, pos)); it != ix.named.end() && it->first == h && it->second < best; ++it) {
        const u64 r = ix.begins[it->second >> 32] + (it->second & 0xffffffffu);
        if (name_equals(record_type(r), w)) { best = it->second; break; }  // a hash collision with another name: skip
    }
    return best;
}
}  // namespace

std::optional<u32> swifttype_read_code(u64 a) {
    if (enabled() && a == kBase + kStepOff) return 0xd4000001u | (kSwiftTypeSvc << 5);  // svc #kSwiftTypeSvc
    return std::nullopt;
}

// At the loop head: x23 = section entry {begin, end}, x24 = end of the section array, x25 = x27 = this record (not
// the section end), x26 = section end, x20 = node.
void swifttype_step(GuestThread &t) {
    u64 sec = t.x(23), rec = t.x(27), end = t.x(26);
    const u64 secend = t.x(24), node = t.x(20);
    if (g_dbg && g_dumps.fetch_add(1) < 4) dump_node(node);
    const Want w = g_filter ? node_name(node) : Want{};
    if (g_dbg) {  // which descriptor kinds carry this node kind's name (whole walk, from this record on)
        const Want a = node_name(node, true);
        if (a.p) {
            std::array<u64, 3> c{};
            u64 s2 = sec, r2 = rec, e2 = end;
            for (;;) {
                for (; r2 != e2; r2 += 4)
                    if (const u64 td = record_type(r2)) {
                        const u32 k = GuestThread::rd<u32>(td) & 0x1f;
                        if (k >= 16 && k <= 18 && name_equals(td, a)) c[k - 16]++;
                    }
                s2 += 16;
                if (s2 == secend) break;
                r2 = GuestThread::rd<u64>(s2);
                e2 = GuestThread::rd<u64>(s2 + 8);
            }
            if (g_dumps < 40) logf("swifttype lookup kind %u name '%.*s'", GuestThread::rd<u16>(node + 16), (int)a.n, a.p);
            std::lock_guard l(g_hist_mu);
            auto &h = g_hist[GuestThread::rd<u16>(node + 16)];
            for (int i = 0; i < 3; i++) h[i] += c[i];
            if ((g_dumps & 63) == 0) {
                std::string out;
                for (auto &[k, v] : g_hist) out += " " + std::to_string(k) + ":" + std::to_string(v[0]) + "/" + std::to_string(v[1]) + "/" + std::to_string(v[2]);
                logf("swifttype kinds (node kind: class/struct/enum descriptors named alike):%s", out.c_str());
            }
        }
    }
    // the index: x22 is the runtime's section array object ({.., storage}; storage = {count, entries[]})
    if (w.p && g_index) {
        const u64 base = GuestThread::rd<u64>(t.x(22) + 8) + 8;
        if (sec >= base && secend >= sec && (secend - base) % 16 == 0 && (sec - base) % 16 == 0) {
            std::lock_guard l(g_ix_mu);
            index_sync(base, (secend - base) / 16);
            const u64 si = (sec - base) / 16;
            if (si < g_ix.begins.size() && g_ix.begins[si] <= rec && rec < g_ix.ends[si] && g_ix.ends[si] == end) {
                u64 nx = index_next(si << 32 | (rec - g_ix.begins[si]), w);
                // an indirect record not bound yet names no type: the guest loop skips it, so do we
                while (nx != ~0ull && !record_type(g_ix.begins[nx >> 32] + (nx & 0xffffffffu))) nx = index_next(nx + 4, w);
                if (nx == ~0ull) {
                    t.set_x(23, secend);
                    t.jit->SetPC(kBase + kDoneOff);
                    if (g_dbg) g_steps++;
                    return;
                }
                const u64 s2 = nx >> 32, r2 = g_ix.begins[s2] + (nx & 0xffffffffu);
                t.set_x(21, record_type(r2));
                t.set_x(23, base + 16 * s2);
                t.set_x(25, r2);
                t.set_x(26, g_ix.ends[s2]);
                t.set_x(27, r2);
                t.jit->SetPC(kBase + kCallOff);
                if (g_dbg) g_calls++;
                return;
            }
        }
    }
    u64 n = 0;
    for (;;) {
        for (; rec != end; rec += 4) {
            n++;
            const u64 td = record_type(rec);
            if (td && may_match(td, w)) {
                t.set_x(21, td);
                t.set_x(23, sec);
                t.set_x(25, rec);
                t.set_x(26, end);
                t.set_x(27, rec);
                t.jit->SetPC(kBase + kCallOff);
                if (g_dbg) { g_recs += n; g_calls++; }
                return;
            }
        }
        sec += 16;
        if (sec == secend) {
            t.set_x(23, sec);
            t.jit->SetPC(kBase + kDoneOff);
            if (g_dbg) {
                g_recs += n;
                if ((++g_steps & 0xff) == 0)
                    logf("swifttype: %llu walks done, %llu records, %llu matcher calls", (unsigned long long)g_steps.load(),
                         (unsigned long long)g_recs.load(), (unsigned long long)g_calls.load());
            }
            return;
        }
        rec = GuestThread::rd<u64>(sec);
        end = GuestThread::rd<u64>(sec + 8);
    }
}

}  // namespace a64
