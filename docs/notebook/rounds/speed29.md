# Speed 29 (2026-11-01): WebContent crash root cause (MADV_ZERO), Swift conformance filter for classes, gate

Phone: var/device-a64-ios272 (iPhone 17, 27.2 24B8117), image $SCRATCH/jc-24B8117 (unchanged). Method: logs first,
mapsroute.sh, openm.sh (frame-timed, app killed first), swiftconf debug counters, guest code disassembly (llvm-mc on the
cache's mappings: $SCRATCH/s29/tools/cdis.py; `ipsw dyld disass` panics on this cache, `ipsw dyld a2s` works).

## 1. Safari WebContent crash in DOMWrapperWorld::clearWrappers: FIXED (root cause: Darling's madvise)
- The crash read a 16-byte bucket {key, WeakImpl*} of the world's wrapper map (unrolled clear loop at
  WebCore +0x...3454) whose value was 0x80000000. In every dump the bad bucket was just past a 4 KB page boundary
  of an 8 KB table (bucket 259, table+0x1030): a "zeroed" allocation whose second page was not zero.
- Darling's sys_madvise (darling-xnu, bsd/impl/mman/madvise.c) returned 0 for every advice >= 5 without doing
  anything. XNU's **MADV_ZERO (11)** zero-fills a range (or fails with ENOTSUP, and the caller zeroes it itself);
  libpas uses it to zero recycled pages. So recycled heap pages kept old contents while WebKit treated them as zero.
- Fix: MADV_ZERO returns ENOTSUP (always correct; callers fall back). darling-xnu 91f5392. The Wikipedia page loads and
  WebContent stays up; gate's Safari check passes with zero crash lines.

## 2. Maps route (AttributeGraph): narrowed, NOT fixed
- New tools: a per-process env file ($SCRATCH/a64-penv, lines "Proc K=V ...", applied by a priority-101 constructor
  in libarm64jit before any knob is read: bisect one app without rebooting), X86CODE_A64_ABORT_STRINGS=1 (C strings
  referenced from the aborting thread's stack), X86CODE_A64_ABORT_MSG=addr, X86CODE_A64_OPT_KEEP=hex.
- Message: `AttributeGraph precondition failure: attribute failed to set an initial value: <id>,
  DynamicLayoutComputer<_VStackLayout>` (nested in an _HStackLayout DynamicLayoutComputer update via
  AGGraphGetValue -> input_value_ref_slow -> update_attribute).
- Still crashes with: both native Swift steps off (2/2), native libc/objc/inline objc/svc fusion/dyld scan all off
  (1/1), strict LDXR/STXR monitor + no image (1/1), Dynarmic IR opts off (GetSet/ConstProp/MiscIROpt) + no image
  (1/1). Default: 1 route shown out of ~9 runs. Guest malloc (X86CODE_A64_GUEST_MALLOC=1) leaves Maps on a blank globe
  (not comparable). Next: find the rule's early return (DynamicLayoutComputer.updateValue specialization in
  SwiftUICore) and what input it reads; compare with a Mac trace of the same steps.
- Gate: `GATE_ROUTE=1 gate.sh` runs mapsroute.sh (own output dir). Off by default until this is fixed.

## 3. Swift conformance scan: class queries filtered natively
- Settled the stack slot: [sp+0xb0] is the **queried metadata** (x0 stored at the function's entry, 0x1c15f5e6c in
  27.2). On a protocol match the guest: kind 0/1 records (type descriptor) -> get the queried type's descriptor,
  equalContexts, then the superclass chain comparing each descriptor; kind 2/3 (ObjC) -> metadata compare with the
  chain. (Speed 28's "compares with the descriptor" was the kind-2/3 metadata compare.)
- swiftconf.cpp make_query: value types as before; classes: walk superclass (+8), Swift bit in the data word (+0x20),
  description (+0x40); usable only if every descriptor is unique and the chain ends at a **non-Swift root**
  (SwiftObject/NSObject): an incomplete superclass leaves the chain ending at a Swift class and nothing is skipped.
  A kind 0/1 record with a unique descriptor outside the set cannot match. X86CODE_A64_SWIFT_CONF_CLASS=0 off.
- Verify mode X86CODE_A64_SWIFT_CONF_VERIFY=1 (needs LIVE_ONLY for the process: the hooked sites are in image
  blocks): never skips; hooks the guest's three "type matched" sites (mov x8,#0 at +0xf0/+0x18c/+0x20c of the walk,
  svc #0xfff9) and logs any match on a record the filter would have skipped. Preferences + Health cold: 96 guest
  matches checked, 0 would have been dropped; Wallet/Maps: 0 fails.
- Guest round trips, cold Settings: 10.0M -> 7.2M (-29%). Frame-timed, killed first (content, s):
  | App | class filter off | on |
  |---|---|---|
  | Settings | 3.62 / 3.55 | 3.49 / 3.83 |
  | Health | 5.43 / 5.56 | 4.58 / 4.47 |
  Settings within noise; Health -0.95 s. Remaining 7.2M: ObjC-named records (kind 2/3), non-unique descriptors, and
  chains ending at a Swift class.

## 4. mach_msg measurement: tool added (X86CODE_A64_MSG_STATS=1), see below for numbers

## 5. Not done this round
- Persisting app/non-cache code per binary, hot/cold layout, native ASTC/LZFSE SplashBoard decode, Siri audio input.
- Huge pages: still blocked by the host (no free order-9 blocks; host settings not changed).

## Gate
See the end of this file.

## 4 (numbers). mach_msg (X86CODE_A64_MSG_STATS=1, counts in 512 steps)
- SpringBoard idle: ~810 mach_msg calls/s (9.7K per 12 s). Cold Settings (URL open): SpringBoard +~2K over idle in
  12 s, Preferences ~2.5-3K in its first 12 s. Warm open: SpringBoard at its idle rate. Time inside the thunk is mostly
  blocking receives, so it is not a latency figure.
- Recommendation: first find what SpringBoard's idle 800 msg/s are (per-port/msgh_id histogram), since that is a constant
  host tax; a darlingserver-bypass fast path for simple same-host send/receive pairs comes after that.

## Gate result
s29g1, s29g2, s29g3 (cold boots, default config): **0 failed each**. Three consecutive clean boots. The Maps route
check isn't in the default gate yet (GATE_ROUTE=1).
