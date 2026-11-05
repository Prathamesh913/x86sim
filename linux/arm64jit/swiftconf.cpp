// x86Code Speed 7: native step for the Swift runtime's protocol conformance scan (iOS 27.0 24A434 libswiftCore).
// swift_conformsToProtocolMaybeInstantiateSuperclasses walks every conformance record of every loaded image on each
// (type, protocol) cache miss, for each superclass: ~10 guest instructions per record, ~11% of Preferences cold.
// The record step at kStep (ldrsw x8,[x23] .. compare the record's protocol with x21) is read by the JIT as
// "svc #kSwiftConfSvc" (MemoryReadCode; guest memory is not written, so the AOT builder sees the same code and the
// block, a host call, is never persisted). The handler runs the same walk natively: the same records in the same
// order, the same relative/indirect protocol resolution, and leaves the registers and the stack slot as the guest
// loop does on reaching the match path (kMatch) or the end of the section list (kDone).
// Records are found through one index (protocol -> records, in walk order), and records that certainly name another
// type are skipped natively (cannot_match); Preferences cold: 51M guest round trips -> 10M, the conformance code
// ~14.5% -> ~3.5% of samples. X86CODE_A64_SWIFT_CONF_NATIVE=0 turns it off, X86CODE_A64_SWIFT_CONF_FILTER=0 only the
// skip. The code words around kStep are checked once before it is used.
#include "a64.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace a64 {
using s32 = int32_t;
using s64 = int64_t;

namespace {
// Speed 25: the step's addresses are found in libswiftCore by its code (swift_text_find), so every 27.x build gets it
// (27.0: kNextSection 0x1c162626c; 27.2: 0x1c15f6ae0). They were 27.0's constants, so 27.1/27.2 ran the whole walk
// in the guest. kStep = +0x3c (loop: ldrsw x8, [x23]), kMatch = +0x6c (add x8, x27, #4: protocol matched), kDone = the
// target of the "b.eq" at +0x10 (every section scanned).
u64 kStep = 0, kNextSection = 0, kMatch = 0, kDone = 0;
// kNextSection .. kMatch (the words of 27.0 and 27.2; the branches are relative, so they match too)
constexpr u32 kExpect[] = {
    0xf94047e8, 0x91004108, 0xf9403be9, 0xeb09011f, 0x54001ae0, 0xa9405117, 0xf90047e8, 0xeb1402ff,
    0x540000e1, 0x17fffff7, 0xeb15011f, 0x54000200, 0x910012f7, 0xeb1402ff, 0x54fffe40, 0xb98002e8,
    0x8b17011b, 0xb9800369, 0x340000c9, 0x927ff928, 0x8b080368, 0x3607fea9, 0xf9400108, 0x17fffff3,
    0xd2800008, 0xeb15011f, 0x54fffe41, 0x91001368,
};
int g_state = -1;  // -1 unchecked, 0 off, 1 on
const bool g_verify = getenv("X86CODE_A64_SWIFT_CONF_VERIFY") != nullptr;
u64 kMatched[3] = {0, 0, 0};  // the guest's "mov x8, #0" right after a successful type check (verify mode)

bool enabled() {
    static const bool ok = [] {
        const char *e = getenv("X86CODE_A64_SWIFT_CONF_NATIVE");
        if (e && atoi(e) == 0) return false;
        kNextSection = swift_text_find(kExpect, nullptr, sizeof kExpect / 4);
        if (!kNextSection) { logf("swift conformance step: code not found in libswiftCore; not used"); return false; }
        kStep = kNextSection + 0x3c;
        kMatch = kNextSection + 0x6c;
        const u32 beq = swift_text_word(kNextSection + 0x10);  // b.eq kDone
        kDone = kNextSection + 0x10 + (u64)((s64)((s32)(beq << 8) >> 13) * 4);
        if (g_verify) {  // 27.2: +0xf0 / +0x18c / +0x20c, each "mov x8, #0"
            const u64 off[3] = {0xf0, 0x18c, 0x20c};
            for (int i = 0; i < 3; i++)
                if (swift_text_word(kNextSection + off[i]) == 0xd2800008u) kMatched[i] = kNextSection + off[i];
            logf("swiftconf verify: matched sites %#llx %#llx %#llx", (unsigned long long)kMatched[0],
                 (unsigned long long)kMatched[1], (unsigned long long)kMatched[2]);
        }
        return true;
    }();
    g_state = ok;
    return ok;
}
}  // namespace

// One index over the whole section list (the runtime's snapshot: an array of {begin, end}, count in [sp+0x38], end in
// [sp+0x70] for the whole walk): protocol -> (section number, record offset) in walk order. Built when the same
// snapshot (array address and count) is seen on two walks in a row (images loading in a burst: walk natively), then
// published immutable; lookups are lock-free.
struct Index {
    u64 base, count;
    std::vector<u64> begins, ends;
    std::unordered_map<u64, std::vector<std::pair<u32, u32>>> by_proto;
};
std::atomic<Index *> g_index{nullptr};
// RAM 11: a rebuilt index replaces the old one, which a reader on another thread may still be walking. They used to
// stay allocated for good: Health and healthappd load images in bursts and kept ~10 of them (70-90 MB of host glibc,
// the by_proto vectors). Readers now publish the index they use in a per-thread hazard slot; a replaced index is freed
// once no slot holds it (checked after each build and, while some wait, on the lookup path).
struct Hazard {
    std::atomic<const Index *> p{nullptr};
    std::atomic<bool> used{false};
};
std::mutex g_hz_mu;
std::vector<Hazard *> g_hz;          // slots are reused, never freed (one per thread alive at once)
std::vector<Index *> g_retired;      // under g_mu
std::atomic<bool> g_have_retired{false};
struct HazardRef {
    Hazard *h = nullptr;
    ~HazardRef() { if (h) { h->p.store(nullptr, std::memory_order_release); h->used.store(false, std::memory_order_release); } }
};
thread_local HazardRef t_hz;
Hazard &my_hazard() {
    if (!t_hz.h) {
        std::lock_guard<std::mutex> lk(g_hz_mu);
        for (Hazard *h : g_hz)
            if (!h->used.load(std::memory_order_acquire)) { h->used.store(true); t_hz.h = h; break; }
        if (!t_hz.h) { t_hz.h = new Hazard; t_hz.h->used.store(true); g_hz.push_back(t_hz.h); }
    }
    return *t_hz.h;
}
// under g_mu: free the retired indexes no hazard slot holds
void reclaim_locked() {
    std::lock_guard<std::mutex> lk(g_hz_mu);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    size_t k = 0;
    for (Index *r : g_retired) {
        bool held = false;
        for (Hazard *h : g_hz) held = held || h->p.load(std::memory_order_seq_cst) == r;
        if (held) g_retired[k++] = r;
        else delete r;
    }
    g_retired.resize(k);
    g_have_retired.store(k != 0, std::memory_order_release);
}
std::atomic<u64> g_seen_base{0}, g_seen_count{0};
std::mutex g_mu;
// X86CODE_A64_SWIFT_CONF_DEBUG=1: counters, logged every 16K steps
const bool g_dbg = getenv("X86CODE_A64_SWIFT_CONF_DEBUG") != nullptr;
std::atomic<u64> g_calls{0}, g_hits{0}, g_builds{0}, g_nobase{0}, g_mism{0}, g_skipped{0};
thread_local u64 t_would_skip = 0;  // verify mode: the record handed to kMatch that the filter would have skipped
std::atomic<u64> g_verify_bad{0}, g_verify_ok{0};
#define COUNT(c) do { if (g_dbg) (c)++; } while (0)
bool getenv_off_filter() {
    static const bool off = getenv("X86CODE_A64_SWIFT_CONF_FILTER") && atoi(getenv("X86CODE_A64_SWIFT_CONF_FILTER")) == 0;
    return off;
}

u64 record_protocol(u64 rec, u64 &desc, s64 &pv) {
    desc = rec + (s64)*(const s32 *)rec;
    pv = *(const s32 *)desc;
    u64 proto = 0;
    if (pv) {
        proto = desc + (u64)(pv & ~s64(1));
        if (pv & 1) proto = *(const u64 *)proto;
    }
    return proto;
}

// The type check the guest runs on a protocol match (kMatch..), decided natively only where it is certain to fail:
// the queried type (metadata in [sp+0xb0]) is a struct/enum/optional (descriptor at +8, no superclass walk) and the
// record names a different type by descriptor (TypeReferenceKind 0 direct / 1 indirect), both descriptors unique
// (ContextDescriptorFlags bit 6): equalContexts of two distinct unique descriptors is false. Anything else (classes,
// ObjC names, non-unique/foreign descriptors) goes to the guest.
u64 unique_value_descriptor(u64 metadata) {
    if (!metadata) return 0;
    const u64 kind = *(const u64 *)metadata;
    if (kind != 0x200 && kind != 0x201 && kind != 0x202) return 0;
    const u64 d = *(const u64 *)(metadata + 8);
    return d && (*(const u32 *)d & 0x40) ? d : 0;
}
// Speed 29: class queries. [sp+0xb0] is the queried metadata (x0 stored at the function's entry). For a record naming
// a type by descriptor (kind 0/1) the guest gets the queried type's descriptor and, failing equalContexts, walks the
// superclass chain comparing each one's descriptor (kind 2/3 records, ObjC class names, compare metadata: never
// skipped here). So a record certainly fails when its unique descriptor is none of the chain's descriptors, all of
// them unique. The chain is read from the metadata (superclass at +8, Swift-class bit in the data word at +0x20,
// description at +0x40) and used only if it ends at a non-Swift root (SwiftObject/NSObject): a superclass the guest
// would still have to instantiate (incomplete metadata) leaves the chain ending at a Swift class, and nothing is
// skipped. X86CODE_A64_SWIFT_CONF_CLASS=0 turns the class part off; X86CODE_A64_SWIFT_CONF_VERIFY=1 never skips but
// logs any record the filter would have skipped that the guest then matched (the guest's three "matched" sites).
struct Query {
    int n = 0;          // descriptors in d (0: no filter)
    u64 d[32];
};
Query make_query(u64 metadata) {
    Query q;
    if (const u64 v = unique_value_descriptor(metadata)) { q.d[q.n++] = v; return q; }
    static const bool cls = !(getenv("X86CODE_A64_SWIFT_CONF_CLASS") && atoi(getenv("X86CODE_A64_SWIFT_CONF_CLASS")) == 0);
    if (!cls || !metadata || *(const u64 *)metadata <= 0x7ff) return q;  // not a class (isa pointer), or off
    bool swift = false;
    int n = 0;
    for (u64 c = metadata; c; c = *(const u64 *)(c + 8)) {
        swift = (*(const u64 *)(c + 0x20) & 3) != 0;
        if (!swift) continue;
        const u64 d = *(const u64 *)(c + 0x40);
        if (!d || !(*(const u32 *)d & 0x40) || n == 32) return q;
        q.d[n++] = d;
    }
    if (swift || !n) return q;  // chain ends at a Swift class (maybe incomplete), or no Swift class at all
    q.n = n;
    return q;
}
bool cannot_match(u64 desc, const Query &q) {
    if (!q.n) return false;
    const u32 kind = (*(const u32 *)(desc + 0xc) >> 3) & 7;
    if (kind > 1) return false;
    const u64 field = desc + 4;
    const s64 rel = *(const s32 *)field;
    if (!rel) return false;
    const u64 td = kind == 0 ? field + rel : *(const u64 *)(field + rel);
    if (!td || !(*(const u32 *)td & 0x40)) return false;
    for (int i = 0; i < q.n; i++)
        if (q.d[i] == td) return false;
    return true;
}

// the current index for this snapshot (protected by the caller's hazard slot h until it clears it), or nullptr
const Index *index_for(u64 base, u64 count, Hazard &h) {
    Index *ix;
    do {
        ix = g_index.load(std::memory_order_seq_cst);
        h.p.store(ix, std::memory_order_seq_cst);
    } while (ix != g_index.load(std::memory_order_seq_cst));
    if (ix && ix->base == base && ix->count == count) {
        if (g_have_retired.load(std::memory_order_relaxed) && g_mu.try_lock()) { reclaim_locked(); g_mu.unlock(); }
        return ix;
    }
    h.p.store(nullptr, std::memory_order_release);
    const bool new_base = g_seen_base.exchange(base) != base, new_count = g_seen_count.exchange(count) != count;
    if (new_base || new_count) return nullptr;  // first sighting of this snapshot
    std::lock_guard<std::mutex> lk(g_mu);
    ix = g_index.load(std::memory_order_acquire);
    if (ix && ix->base == base && ix->count == count) { h.p.store(ix, std::memory_order_seq_cst); return ix; }
    auto *n = new Index{base, count, {}, {}, {}};
    for (u64 i = 0; i < count; i++) {
        const u64 b = *(const u64 *)(base + 16 * i), e = *(const u64 *)(base + 16 * i + 8);
        n->begins.push_back(b);
        n->ends.push_back(e);
        for (u64 r = b; r != e; r += 4) {
            u64 d;
            s64 pv;
            n->by_proto[record_protocol(r, d, pv)].emplace_back((u32)i, (u32)(r - b));
        }
    }
    COUNT(g_builds);
    h.p.store(n, std::memory_order_seq_cst);
    g_index.store(n, std::memory_order_seq_cst);
    if (ix) g_retired.push_back(ix);
    reclaim_locked();
    return n;
}

std::optional<u32> swiftconf_read_code(u64 a) {
    if (enabled() && a == kStep) return 0xd4000001u | (kSwiftConfSvc << 5);  // svc #kSwiftConfSvc
    if (g_verify && g_state == 1 && (a == kMatched[0] || a == kMatched[1] || a == kMatched[2]))
        return 0xd4000001u | (kSwiftConfVerifySvc << 5);
    return std::nullopt;
}

// verify mode: the guest matched the record in x23; it must not be one the filter would have skipped
void swiftconf_verify(GuestThread &t) {
    t.set_x(8, 0);  // the replaced "mov x8, #0"
    if (t_would_skip && t.x(23) == t_would_skip) {
        u64 d;
        s64 pv;
        record_protocol(t_would_skip, d, pv);
        logf("swiftconf VERIFY FAIL: record %#llx (conformance %#llx) matched, the filter would skip it; metadata %#llx",
             (unsigned long long)t_would_skip, (unsigned long long)d, (unsigned long long)*(const u64 *)(t.sp() + 0xb0));
        g_verify_bad++;
    } else g_verify_ok++;
    if (const u64 nn = g_verify_ok + g_verify_bad; (nn & (nn - 1)) == 0)
        logf("swiftconf verify: %llu matches checked, %llu would have been dropped", (unsigned long long)g_verify_ok.load(),
             (unsigned long long)g_verify_bad.load());
}

void swiftconf_step(GuestThread &t) {
    t_would_skip = 0;
    u64 rec = t.x(23), end = t.x(20);
    const u64 want = t.x(21), sp = t.sp();
    const u64 last = *(const u64 *)(sp + 0x70), count = *(const u64 *)(sp + 0x38);
    const u64 base = last - 16 * count;
    u64 entry = *(const u64 *)(sp + 0x88);
    if (g_dbg && (++g_calls & 0x3fff) == 0)
        logf("swiftconf: %llu calls, %llu skipped, %llu indexed, %llu builds, %llu no index, %llu mismatched (count %llu base %#llx)",
             (unsigned long long)g_calls.load(), (unsigned long long)g_skipped.load(), (unsigned long long)g_hits.load(), (unsigned long long)g_builds.load(),
             (unsigned long long)g_nobase.load(), (unsigned long long)g_mism.load(), (unsigned long long)count, (unsigned long long)base);
    Hazard &hz = my_hazard();
    struct Clear { Hazard &h; ~Clear() { h.p.store(nullptr, std::memory_order_release); } } clear{hz};
    const Index *ixp = count && entry >= base && entry < last ? index_for(base, count, hz) : nullptr;
    if (!ixp) COUNT(g_nobase);
    if (const Index *ix = ixp) {
        const u32 si = (u32)((entry - base) / 16);
        if (ix->begins[si] != *(const u64 *)entry || ix->ends[si] != end) COUNT(g_mism);
        else {
            COUNT(g_hits);
            const auto it = ix->by_proto.find(want);
            const std::pair<u32, u32> from{si, (u32)(rec - ix->begins[si])};
            const auto *v = it == ix->by_proto.end() ? nullptr : &it->second;
            auto at = v ? std::lower_bound(v->begin(), v->end(), from) : decltype(v->begin()){};
            if (v && !getenv_off_filter()) {
                const Query q = make_query(*(const u64 *)(sp + 0xb0));
                for (; at != v->end(); ++at) {
                    u64 d;
                    s64 pv;
                    record_protocol(ix->begins[at->first] + at->second, d, pv);
                    if (!cannot_match(d, q)) break;
                    if (g_verify) { t_would_skip = ix->begins[at->first] + at->second; break; }
                    COUNT(g_skipped);
                }
            }
            if (!v || at == v->end()) {  // no further match: as the walk ends
                *(u64 *)(sp + 0x88) = last - 16;
                t.set_x(8, last);
                t.set_x(9, last);
                t.jit->SetPC(kDone);
                return;
            }
            if (at->first != si) {
                entry = base + 16 * at->first;
                *(u64 *)(sp + 0x88) = entry;
                end = ix->ends[at->first];
                t.set_x(20, end);
            }
            rec = ix->begins[at->first] + at->second;  // the walk's next match; checked below like any record
        }
    }
    for (;;) {
        for (; rec != end; rec += 4) {
            u64 desc;
            s64 pv;
            const u64 proto = record_protocol(rec, desc, pv);
            if (proto == want) {
                t.set_x(23, rec);
                t.set_x(27, desc);
                t.set_x(9, (u64)pv);
                t.set_x(8, proto);
                t.jit->SetPC(kMatch);
                return;
            }
        }
        // kNextSection: next section entry {begin, end}
        const u64 sec = *(const u64 *)(sp + 0x88) + 16;
        if (sec == last) {
            t.set_x(8, sec);
            t.set_x(9, last);
            t.jit->SetPC(kDone);
            return;
        }
        *(u64 *)(sp + 0x88) = sec;
        rec = *(const u64 *)sec;
        end = *(const u64 *)(sec + 8);
        t.set_x(20, end);
    }
}

}  // namespace a64
