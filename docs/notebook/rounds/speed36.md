# Speed 36 (2026-11-02): Maps route fixed (shared vm_remap keeps aliasing), mach_msg traffic census, in-slot split

Phone: var/device-a64-ios272 (iPhone 17, 27.2 24B8117), image $SCRATCH/jc-24B8117 (codegen 0x1a). Method: logs first.

## 1. Maps route: darlingserver's shared vm_remap, piece by piece (default)
- Root cause (speed35.md §5): AttributeGraph's node table grows by `vm_remap(copy=FALSE)` of the old table into a 4x
  region. The second growth's source is the first growth's destination: the old table's memfd followed by fresh
  anonymous pages. dtape_share_region saw a MAP_SHARED memfd mapping that did not cover the range and treated it as a
  file mapping (`shared && !anon_shared` -> snapshot): the new table got a copy, so stores through pointers computed
  before the rule (x28) landed in the old table and the update reported "failed to set an initial value".
- Fix (external/darling, duct-tape memory.c): `mach_vm_remap_external_shared` remaps a source made of several
  mappings piece by piece (`mach_vm_remap_external_shared_pieces`). Each piece is shared as it is: a memfd piece keeps
  its memfd (every earlier alias keeps sharing pages with the new range, as on XNU), a private piece is moved into a
  new memfd as before, a file piece is still a snapshot. The first piece places the whole range (ANYWHERE / FIXED /
  OVERWRITE as before); later pieces overwrite their part. One shared entry per piece in the target map.
- Speed 30's REMAP_FREEZE (opt-in, unchanged default): its check looked at the first mapping only, so a multi-piece
  source (first piece shared) was never frozen. It now checks the whole range (common protection rw, any private
  piece) and freezes it all. Still opt-in: the race it closes did not show up once aliasing was right.
- Verified with X86CODE_A64_AGCHK=1 X86CODE_A64_LIVE_ONLY=Maps (a64-penv, Maps only): 39 "TABLE MOVED" checks, every
  one with the same flags through the old pointer and the new base (e.g. node(x28) 0x3cf3de4f4 flags 0x3073 | node
  now 0x4427f64f4 flags 0x3073), no ABORT PATH (Speed 35: 2 of 2 runs aborted).
- mapsroute.sh: 12 runs (10 default + 2 probe): the route showed in 11. The 12th (s36r8) ended on the place card's
  "Add a Note" sheet with Maps alive and no abort in its log: the OCR'd "Plan" tap hit the wrong control. mapsroute.sh
  now confirms the Directions sheet after Plan (closes the sheet and retries otherwise).
- GATE_ROUTE is on by default (GATE_ROUTE=0 skips it).

## 2. mach_msg fast path: traffic census first; not built
New opt-in counters in darlingserver (call.cpp / kqchan.cpp / mach_msg.c): while
$SCRATCH/ds-rpcstats.on exists, per-second counts of every RPC by call number, kqchan notify / read / modify, the
shape of each sent mach_msg, and, for simple messages, where the destination's receive right is. Cost when off: one
clock read per RPC and an int test per send. Logged to $SCRATCH/ds-rpcstats.log ($SCRATCH/s36/rpcstats-run1.log
and the second run kept; script $SCRATCH/s36/count.sh).

RPC rate: idle 1.6k/s, warm taps 4.1k/s, cold opens 3.4k/s. Warm tap phase (55 s, 5 taps): mach_msg 110.8k,
port_deallocate 28.7k, kqchan notify 14.8k + read 14.5k, port set insert/extract_member 11.3k each, mk_timer_arm 7.4k,
psynch mutex/cv ~3k each, semaphores ~2k.

Shapes of sends (tap phase): complex send+receive (MIG/XPC requests with port or OOL descriptors) **44.7k of 78.5k**;
simple, no reply port, send only 21.9k; simple with a send-once reply port 5.4k (send) + 3.4k (send+receive);
complex send-only 3.2k; vouchers 0.

Where simple messages go (receive right of the destination):

| | idle | warm taps | cold opens |
|---|---|---|---|
| workloop / knote (libdispatch), other task | 13% | 53% | 53% |
| port set, same task | 7% | 19% | 26% |
| port set, other task | 8% | 16% | 13% |
| kernel object | 56% | 5% | 2% |
| plain receive, same task | 15% | 6% | 3% |
| plain receive, other task | 0% | 0.3% | 3% |

Conclusion: the speed35 design's first cut (simple messages, plain receive) covers 1-2% of real mach_msg traffic
(simple messages are a third of sends, and 3-9% of those reach a plain receive). It would speed msgbench (which is
in-process, plain receive) and nothing measurable on the phone, while adding a second delivery path with ordering
risks. Not built. What a fast path has to cover to matter, in order:
1. complex send+receive with a send-once reply (the MIG/XPC request pattern): needs right transfer in the fast
   path, i.e. port names outside darlingserver;
2. delivery to workloop / knote receivers (libdispatch), which today costs a send RPC plus a kqchan notify and a
   read round trip;
3. port sets (CFRunLoop): also 11k insert/extract_member RPCs per tap phase, probably mode switches.
Upper bound for all of it: ~4k RPCs/s x ~10 us (client + server) = ~40 ms/s of CPU during taps, against
SpringBoard's own 540-620 ms per tap. msgbench / idle CPU / taps were not re-measured with a fast path (none built).

## 3. Split inside the existing slots
A chunk's block list goes to the builder sorted by profile count, hottest first (hot::in_ranges), and a truncated
slot drops the coldest blocks (Speed 33). So within every slot the layout already is hot head first, cold tail
after: the in-slot split is today's image. What Speed 32's split adds is cross-library contiguity (all hot heads
before all cold tails), which needs code to move between slots; with the slab full (1224 MB) that needs a full build,
and a full build loses ~0.6M blocks of coverage (speed35.md §2). Isolating the layout effect therefore needs a full
build with the same coverage (a larger slab or a tighter cold tail), not done.

## Measurements (default config + this round's darlingserver, fresh boot; census counters on)
- Maps route: before 0 of 2 (Speed 35, aborted every time); after 11 of 12 runs + 3 of 3 in-gate (the miss was the
  test's tap, fixed).
- Warm taps x5: response 0.418-0.637 s (first 0.637, rest 0.418-0.478), content 0.737-0.93 s, SB CPU 540-620 ms
  (Speed 35: 530-550 ms). Cold Settings 3.78 / 3.74 s (Speed 35 3.51 / 3.94), x86ui 1.77 (one 6.68 s outlier, a
  5.3 s response wait), Cadence 2.33 / 1.80, warm 0.72-0.87 s. Neutral, as expected: the remap fix touches one
  AttributeGraph growth per Maps process (dserver.log "piecewise shared remap": 1 per route run, none in Safari).

## Gate
- s36g1 clean; s36g2 failed maps-tiles: the map was drawn (central Paris, where Maps reopens after the route test),
  maptiles.py's blue+green threshold assumed a wide view. It now also passes many-colour street views.
- s36h1 clean; s36h2: Safari STALL (its WebContent stuck from launch, still alive 9 s after SIGKILL; its JIT log stops
  after dyld_sim prepare) then Maps not up within 60 s for the route. The new remap path was never taken by
  WebKit (dserver.log); 6 Safari opens on a fresh boot afterwards all loaded. Not seen in earlier kept gate logs:
  open, watch for it.
- **s36i1, s36i2, s36i3: 0 failed each, 3 consecutive clean** (route included). tests/apps/run.sh **PASS**
  (run-20261102-085418).
- Seen meanwhile, pre-existing: com.apple.WebKit.GPU SIGSEGV at an mldr address during SIGKILL teardown of Safari
  (same signature in Nov 1 logs).

## Defaults / opt-ins
- New defaults (gated): piecewise shared vm_remap in darlingserver; GATE_ROUTE=1; maptiles / mapsroute checks.
- Opt-in: darlingserver census ($SCRATCH/ds-rpcstats.on), X86CODE_A64_REMAP_FREEZE=1 (now whole-range),
  X86CODE_A64_AGCHK=1.
- Not built: mach_msg fast path (§2). Not done: layout-only A/B (§3).

Rule check: nothing deleted (the previous darlingserver is in $SCRATCH/trash/darlingserver.s35); a64-penv cleared;
census flag file removed; phone left booted at home in the default config.
