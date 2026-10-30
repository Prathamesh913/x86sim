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
constexpr u64 kStep = 0x1c16262a8;    // loop: ldrsw x8, [x23]
constexpr u64 kNextSection = 0x1c162626c;
constexpr u64 kMatch = 0x1c16262d8;   // add x8, x27, #4 (protocol matched)
constexpr u64 kDone = 0x1c16265d8;    // every section scanned
// kNextSection .. kMatch
constexpr u32 kExpect[] = {
    0xf94047e8, 0x91004108, 0xf9403be9, 0xeb09011f, 0x54001ae0, 0xa9405117, 0xf90047e8, 0xeb1402ff,
    0x540000e1, 0x17fffff7, 0xeb15011f, 0x54000200, 0x910012f7, 0xeb1402ff, 0x54fffe40, 0xb98002e8,
    0x8b17011b, 0xb9800369, 0x340000c9, 0x927ff928, 0x8b080368, 0x3607fea9, 0xf9400108, 0x17fffff3,
    0xd2800008, 0xeb15011f, 0x54fffe41, 0x91001368,
};
int g_state = -1;  // -1 unchecked, 0 off, 1 on

bool enabled() {
    if (g_state < 0) {
        const char *e = getenv("X86CODE_A64_SWIFT_CONF_NATIVE");
        bool ok = !(e && atoi(e) == 0);
        for (size_t i = 0; ok && i < sizeof kExpect / 4; i++) ok = GuestThread::rd<u32>(kNextSection + 4 * i) == kExpect[i];
        g_state = ok;
        if (!ok && !(e && atoi(e) == 0)) logf("swift conformance step: code mismatch at %#llx; not used", (unsigned long long)kStep);
    }
    return g_state == 1;
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
std::atomic<u64> g_seen_base{0}, g_seen_count{0};
std::mutex g_mu;
// X86CODE_A64_SWIFT_CONF_DEBUG=1: counters, logged every 16K steps
const bool g_dbg = getenv("X86CODE_A64_SWIFT_CONF_DEBUG") != nullptr;
std::atomic<u64> g_calls{0}, g_hits{0}, g_builds{0}, g_nobase{0}, g_mism{0}, g_skipped{0};
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
bool cannot_match(u64 desc, u64 qdesc) {
    if (!qdesc) return false;
    const u32 kind = (*(const u32 *)(desc + 0xc) >> 3) & 7;
    if (kind > 1) return false;
    const u64 field = desc + 4;
    const s64 rel = *(const s32 *)field;
    if (!rel) return false;
    const u64 td = kind == 0 ? field + rel : *(const u64 *)(field + rel);
    return td && td != qdesc && (*(const u32 *)td & 0x40);
}

const Index *index_for(u64 base, u64 count) {
    Index *ix = g_index.load(std::memory_order_acquire);
    if (ix && ix->base == base && ix->count == count) return ix;
    const bool new_base = g_seen_base.exchange(base) != base, new_count = g_seen_count.exchange(count) != count;
    if (new_base || new_count) return nullptr;  // first sighting of this snapshot
    std::lock_guard<std::mutex> lk(g_mu);
    ix = g_index.load(std::memory_order_acquire);
    if (ix && ix->base == base && ix->count == count) return ix;
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
    g_index.store(n, std::memory_order_release);  // the old one stays allocated: a reader may still use it
    return n;
}

std::optional<u32> swiftconf_read_code(u64 a) {
    if (a == kStep && enabled()) return 0xd4000001u | (kSwiftConfSvc << 5);  // svc #kSwiftConfSvc
    return std::nullopt;
}

void swiftconf_step(GuestThread &t) {
    u64 rec = t.x(23), end = t.x(20);
    const u64 want = t.x(21), sp = t.sp();
    const u64 last = *(const u64 *)(sp + 0x70), count = *(const u64 *)(sp + 0x38);
    const u64 base = last - 16 * count;
    u64 entry = *(const u64 *)(sp + 0x88);
    if (g_dbg && (++g_calls & 0x3fff) == 0)
        logf("swiftconf: %llu calls, %llu skipped, %llu indexed, %llu builds, %llu no index, %llu mismatched (count %llu base %#llx)",
             (unsigned long long)g_calls.load(), (unsigned long long)g_skipped.load(), (unsigned long long)g_hits.load(), (unsigned long long)g_builds.load(),
             (unsigned long long)g_nobase.load(), (unsigned long long)g_mism.load(), (unsigned long long)count, (unsigned long long)base);
    const Index *ixp = count && entry >= base && entry < last ? index_for(base, count) : nullptr;
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
                const u64 qdesc = unique_value_descriptor(*(const u64 *)(sp + 0xb0));
                for (; at != v->end(); ++at) {
                    u64 d;
                    s64 pv;
                    record_protocol(ix->begins[at->first] + at->second, d, pv);
                    if (!cannot_match(d, qdesc)) break;
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
