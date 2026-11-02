# Speed 32 (2026-11-02): mach_msg cost in darlingserver, native LZFSE snapshot decode, image layout and size

Phone: var/device-a64-ios272 (iPhone 17, 27.2 24B8117), image $SCRATCH/jc-24B8117. Method: logs first; new
tests/arm64/msgbench.c (mach_msg microbenchmark, built on the Mac like the other tests/arm64 binaries) and
tests/arm64/gate/simcost.sh (CPU of the whole sim = its systemd scope's cpu.stat, darlingserver's own CPU,
SpringBoard's CPU and context switches, at idle or around a command); openm.sh / tapm.sh / perfopen.sh as before.
"before" = the Speed 31 default config on a fresh boot, darlingserver and libarm64jit as committed in Speed 31.

## 1. mach_msg cost (main lever)

### Measured (msgbench.sim, 27.2 runtime, darlingserver of the phone's prefix)
| | Mac (arm64, native) | Darling before | Darling after |
|---|---|---|---|
| round trip (send+rcv to a server thread that replies, MAKE_SEND_ONCE reply port) | 5.2 us | 20.9 us | 17.6-18.2 us |
| one-way ping-pong (2 messages, 4 calls) | 3.2 us | 22.1 us | 21.9 us |
| send + receive on one port, no wakeup | 0.48 us | 12.0 us | 10.6 us |
| null trap (mach_port_type) | 0.15 us | 5.6 us | 5.0 us |

Where a null trap's ~20k cycles go (perf, 3 s at ~177k traps/s): 68% kernel (both sides), darlingserver user ~24%,
client user ~8%. Syscalls per trap: client 6 (sendmsg, recvmsg, 2x rt_sigprocmask, getpid, gettid), darlingserver 5
(recvmmsg x2 of which one ended in EAGAIN, sendmmsg, epoll_wait, process_vm_writev for the copyout). A syscall costs
~33 ns here (getpid; a sigprocmask pair 80 ns), so the syscall count is not the cost: the two cross-process wakeups
are. A plain Linux AF_UNIX datagram ping-pong between two processes takes 2.6 us per round trip on this host; Darling's
null trap is that plus ~2.5 us of darlingserver + client work. A round trip (two RPCs plus the wakeup of the blocked
receiver's RPC) is ~20 us of CPU, 3 context switches in the client per round trip; darlingserver's user time was ~3.6
us per mach_msg RPC.

Busy-polling instead of sleeping (tried on a plain socket pair first): 2.6 -> 0.72 us per round trip when the two
processes sit on different CPUs, but 2.5 -> 8.8 us when they share one (pinned), and bimodal unpinned (0.72 / 6.6 /
8.5 us between runs). During a cold open the host is loaded, so spinning would often hit the shared-CPU case; not done.
A fast path that bypasses darlingserver for simple messages would need the port name spaces outside darlingserver
(every right lives in its duct-taped XNU IPC space); that did not fit the time box. Done instead: darlingserver's own
cost per RPC.

### darlingserver changes (default; in the gate boots)
- `current_thread()` (XNU code calls it everywhere) went through `Thread::currentThread()` (a shared_ptr copy) and
  `impersonatingThread()` (the thread's rwlock + another shared_ptr copy): ~1/3 of darlingserver's user CPU per
  mach_msg RPC (rwlock rdlock/unlock, refcount atomics). Now a raw pointer from the thread-local plus an atomic mirror
  of `_impersonating` (only the thread itself sets what it impersonates, kqchan.cpp; the target stays alive through
  the shared_ptr).
- Microthread switches (dserver_getcontext / setcontext, ~4 per RPC) saved and reloaded the whole x87 environment
  (fnstenv + fldenv, serializing: the top darlingserver symbol after the above). They now keep only the x87 control
  word (fnstcw / fldcw) and MXCSR: the switches happen at call boundaries, where the ABI leaves the x87 stack empty.
- MessageQueue::receiveMany: a recvmmsg batch shorter than its slots means the queue was empty, so the extra pass
  that only returned EAGAIN (one syscall per RPC at low load) is skipped. The listener is edge-triggered; a datagram
  queued after the look raises a new edge.

### On the phone (before -> after, fresh boot each; "after" processes ran the same libarm64jit except LZFSE/live)
| | before | after |
|---|---|---|
| idle, whole sim (3 x 10 s at home) | 20 / 20 / 23 ms/s | 19 / 18 / 19 ms/s |
| idle, darlingserver | 5 / 5 / 5 ms/s | 5 / 3 / 4 ms/s |
| SpringBoard per main-thread wakeup (idle) | 12-13 us CPU | 12-13 us CPU |
| warm Settings tap x5: response | 0.419-0.454, mean 0.428 s | 0.418-0.465, mean 0.439 s |
| warm tap content | 0.739-0.912, mean 0.779 s | 0.740-0.907, mean 0.776 s |
| SpringBoard CPU per tap | 530-670 ms | 530-580 ms |
| darlingserver during the 5 taps | 34 ms/s | 31 ms/s |
| cold opens (killed, 2 rounds; content) Settings / x86ui / Cadence | 3.42, 3.44 / 6.74*, 1.71 / 2.65, 2.15 s | 11.34**, 3.47 / 2.01, 1.74 / 2.08, 2.19 s |
| first Settings open after boot | 3.88 s | 4.08 s |
| darlingserver during the cold-open run | (not recorded) | 27 ms/s |
(*) x86ui's first open of that boot. (**) the image had just been reassembled (libcompression chunk rebuilt during the
boot): the first new process copies the new 1.5 GB image into tmpfs (Speed 28's huge-page copy), ~7 s. Not a
darlingserver effect; the second round is the steady state.
Reading: darlingserver is a small part of what the phone spends: at idle 5 of ~20 ms/s, during taps ~31-34 of 430
ms/s. Cutting its per-RPC user time (rt -13%) moves idle CPU by ~1-3 ms/s and nothing measurable on taps or opens. The
wakeup cost per message is the kernel's (two context switches across processes), which only a different transport
(no server hop) removes.

Final default boot (all of Speed 32's defaults, the image left after the repair below), killed cold, 2 rounds:
Settings 3.81 / 3.86 s (before 3.42 / 3.44), x86ui 1.75 / 1.69 (1.71 second round before), Cadence 1.99 / 1.81
(2.65 / 2.15); first Settings after boot 4.11 s (3.88). Idle sim 21 / 30 / 21 ms/s. The image is now the 1.7 GB one
(4.16M blocks) the install hook built (section 2), so these mix the image change in.

## 2. Speed 31's opt-ins: NOT made default (they passed the gate, then broke the image on installs)
- With X86CODE_A64_AOT_LIVE=1 and X86SIM_APP_AOT=1 as defaults: gate s32i1-3 **3 consecutive clean** and tests/apps
  PASS (22/22). But tests/apps reinstalls 3 apps, and the install hook's `x86code-aot update`:
  - 1st install: 483 chunks were stale through .live gaps (the boots since Speed 31 recorded them), rebuilt into new slots
    at the end: 3.43M blocks translated, image 1.53 -> 1.70 GB (4.16M blocks, 1225 MB code).
  - 2nd and 3rd install: "the slab is full (1289 MB used)": no image, but placeapp had already added the apps'
    placement lines, so placement.txt (the image key) no longer matched any image: **every process ran off the image**
    (live JIT + .bins): cold Settings 4.2-4.7 s, x86ui 3.2-4.1 s, Cadence 3.5-7.0 s, first Settings 6.0 s.
  - Repaired: the two lines added after the image was built were dropped from placement.txt (the old file is in
    $SCRATCH/s32/placement.after-apps.txt); the key matches image 63c583ad again; x86net / x86ui (reinstalled) run live.
  - Both stay opt-in (code back to `=1` to enable). Needed before they can be default: placeapp must not commit the
    placement unless the update fits (or roll it back), and .live staleness must be bounded by the slab (rebuild in
    place, drop cold blocks) instead of re-slotting at the end.
- **Bigger image**: a full build with every accumulated hot block (4.65M without, 5.47M with the new .live files) does
  not fit the 1344 MB slab at all ("slab is full", 1330-1335 MB wanted before any per-process room). So growing the
  image to the whole training set is out; the per-process code-space cap stands. Current image: 1225 MB code.
- Gate change (test only): the Siri check presses the side button once more if no Siri UI process appeared. Evidence:
  with Speed 31's libarm64jit and darlingserver, 2 of 2 fresh boots missed the first long press 150 s after boot and
  caught the second; s32g1 and s32h2 failed on exactly that. Every other check unchanged.

## 3. Hot/cold layout: implemented opt-in, not measurable here (time box)
- In scope "hot" library chunks hold only trained blocks, already hottest first, and no successor is followed (only
  app chunks follow successors), so a split needs no duplicate filter: `hot_split_pct` (aot config, default 0) keeps
  the globally top N% of blocks (by profile count) in each library's chunk and puts the rest into "<name>#cold"
  chunks laid out after every hot head (needs `build --full`).
- With the whole training set it plans 1385 chunks (955 + 430 cold tails), but that set does not fit the slab
  (section 2). Restricted by `train_until` (profile mtimes) the set is either 1.6M blocks, whose counts do not
  separate (no cold tails), or 4.65M (too big). So no A/B; L1i / iTLB numbers stay Speed 31's (79 misses / 1k
  instructions, 5.77M walks). Full builds of a 1.6M-block image take 13 s. Next: count-based training (sampled
  execution counts, not "in how many profiles"), then this split.

## 4. Native LZFSE (default, in the gate boots)
- The snapshot's LZFSE goes through the stream API (ImageIO IIOReadPlugin::decodeLZFSEIntoBuffer ->
  compression_stream_init / _process / _destroy; GOT read from SpringBoard: 0x221ede7b0..7c0). lzfse.cpp: svc at
  compression_decode_buffer, lzfse_decode_buffer (Apple's reference decoder, third_party/lzfse, BSD-3) and at the
  stream entries (LZFSE decode streams get a host-side state; input gathered until the "bvx$" end block, decoded at
  once, handed out as dst has room). Other algorithms / encode run libcompression (entry instruction emulated).
  The libcompression chunk was rebuilt (`x86code-aot update --only libcompression --force`, new --force flag).
  X86CODE_A64_LZFSE_NATIVE=0 off, =verify for the buffer calls.
- Warm Settings taps (fresh SpringBoard each, taps 2-5): native resp 0.393-0.425 mean 0.403 s, content 0.711;
  libcompression 0.385-0.399 mean 0.393, content 0.734; SpringBoard 440 ms CPU per tap both. **No measurable effect**:
  the snapshot's LZFSE payload is small next to the ASTC decode.
- Side finding: the long-running SpringBoard of a gate run used 590-640 ms CPU per tap (response 0.45-0.53 s); after a
  SpringBoard restart 440 ms (0.39-0.43 s). Something accumulates in a long-lived SpringBoard; next round.

## 5. Maps route: one step (X86CODE_A64_AGOUT=1, LIVE_ONLY=Maps, ABORT_TSD; 2 of 3 runs aborted)
AGGraphSetOutputValue's return is now probed (return thunk, every set in the ring). For the failing attribute
(0x3b4958 / 0x3d0158, frame flags 5, the last event of the ring): right after the set returned, its node (table base
[0x22e74c908] + id) went flags word +4 **0xfd63 -> 0xfd73** (bit 0x10 set) and value offset +8 **0 -> 0x3f6930 /
0x3f6330** (allocated); the value's first word read 0. So the store landed in the node. The abort's check therefore
sees a later state (a loss after the set, e.g. the table remap) or tests something else. Next step: read the same
node at the abort (ABORT_TSD dump) and compare with this after-set state.

## Gate
s32g1 (Siri first press) and s32h2 (same) failed; s32h1 clean. After the Siri retry: **s32i1, s32i2, s32i3: 0 failed
each, 3 consecutive clean** (default config incl. the new darlingserver, native LZFSE and, then, AOT_LIVE/APP_AOT);
tests/apps/run.sh PASS ($SCRATCH/apps/run-20261102-010839). Defaults now: darlingserver changes, native LZFSE.
Opt-in: X86CODE_A64_AOT_LIVE, X86SIM_APP_AOT (section 2), hot_split_pct, X86CODE_A64_AGOUT.
Rule slip: while setting up the layout experiment one cache file I had just created (s32/jc-split/aot/hot.bin, the
planner's hot-set cache from my own `status` call there) was removed with rm; nothing else under scratch was deleted.
The phone is left booted at home in the default config; a64-penv is empty.
