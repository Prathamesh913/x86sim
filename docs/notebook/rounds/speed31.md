# Speed 31 (2026-11-01): app code and live-translated blocks in the image, native ASTC snapshot decode

Phone: var/device-a64-ios272 (iPhone 17, 27.2 24B8117), image $SCRATCH/jc-24B8117. Method: logs first, frame-timed opens
(openm.sh; x86ui / Cadence opened by bundle id, t0 = the trigger write), perf split by mapping (new
tests/arm64/gate/nonimage.sh), live translations binned by guest PC (new X86CODE_A64_LIVE_BINS), tapm.sh for warm
taps, perfopen.sh for counters. Every "before" is the same default config on a fresh boot.

## 1. SpringBoard motion engine at idle: Apple's behaviour, left alone (time box used, no change)
UIKitCore 27.2 read with $SCRATCH/s31/tools/cd2.py (llvm-mc per word, branch targets and UIKit symbols):
- `_UIMotionEffectSimulatorMotionEventProvider` produces no events at all. start/stopGeneratingEvents only flip
  `_shouldGenerateEvents` on an NSOperationQueue. `+_eventProviderClass` is that class, constant: same on a Mac.
- `+_motionEffectsEnabled` returns NO only for `_AXSReduceMotionEnabled` (0 on the Mac's sim too) or, on internal
  builds, the UIMotionEffectsEnabled default. `_updateDisplayLinkIntervalForScreenState:` pauses the link only when the
  screen is dimmed. It drops to 5-8 fps only in slow mode, which `_updateSlowModeForNewEvent:...` enters only from
  real events. Without events: 30 fps (60 on >60 Hz screens), each callback returning early.
- Speed 30's Mac comparison was not valid: the Mac's 27.0 phone was in Settings with a Maps location alert
  ($SCRATCH/s31/mac27.png), not at home. Its SpringBoard does ~70 context switches/s. lldb cannot attach without an
  interactive login.
- Conclusion: the 30 fps link is UIKit's behaviour without motion hardware. Changing it would mean changing UIKit's
  behaviour (it counts as feature disabling), so it is not done. The lever stays the cost per wakeup (mach_msg via
  darlingserver).

## 2. App code and live-translated library code in the AOT image (main lever)
Where a cold app open's live JIT went (X86CODE_A64_LIVE_BINS: blocks translated live by guest PC; perf split):
| | live blocks: dyld cache / placed / other (app) | perf: image / live JIT / libarm64jit |
|---|---|---|
| x86ui before | 11306 / 2 / 2772 | 54.6 / 14.3 / 25.2 % |
| Cadence before | 20092 / 81 / 6195 | 46.3 / 13.5 / 34.5 % |
| x86ui after | 395 / 2 / 627 | 58.8 / 12.5 / 22.1 % |
| Cadence after | 637 / 9 / 634 | 57.2 / 10.9 / 25.3 % |
| Settings before / after | (783 persistable) | 49.4 / 13.5 / 31.7 -> 49.9 / 14.2 / 30.1 % |
Most of the live code was dyld-cache code the image lacked (untrained paths), not the apps' own code. What stays
"live JIT" in perf is executing never-persisted svc/HostCall blocks (Settings: the native Swift steps), not
translating.
- **App tier fixed** (it never worked): the AOT builder skipped every `Containers/` placement key (placement_map_all),
  so app chunks would have faulted. It now maps them from the device's data dir (X86CODE_A64_AOT_DEVICE_DATA, set by
  x86code-aot from config.device). App chunks are kept in "hot" scope and translated whole (static entries, slot =
  max(est, 12x code bytes): a debug build's Cadence.debug.dylib is 69k blocks / 15.7 MB). They are keyed by Mach-O UUID +
  FNV-64 of the file + slot (debug builds keep their UUID across rebuilds).
- **cli/libexec/arm64/placeapp.py**: an installed app's Mach-Os (executable, dylibs, plug-ins) get slots after the
  highest existing one. No existing slot moves (mkplacement.py packs in path order: a Containers/ key would shift every
  System/ slot). The window has ~32 MB left. `X86SIM_APP_AOT=1 x86sim install ...` runs it plus `x86code-aot update`
  (opt-in).
- **Training feedback from processes on the image** (X86CODE_A64_AOT_LIVE=1, opt-in): such a process never wrote a .bin
  (Speed 9), so what the image lacked was translated live on every open, forever. It now keeps
  `<key>.<exe>.live`: the union of the persistable descriptors it translated itself (descriptor-only A64JC06 shape,
  merged on each save; dynarmic Jit::GetDynamicDescriptors). The hot set reads them ranked first (hot::LIVE_RANK), so a
  full slot drops colder blocks instead. A chunk is stale when >= 32 of its .live blocks are missing from its chunk
  file. Other training growth does not mark chunks stale: the 4.6M accumulated hot blocks would push the image code
  from ~704 to ~960+ MB and leave too little slab for each process's own code.
- This round's image: 32 chunks rebuilt (23 libraries with live gaps, the 7 x86ui / Cadence files, AlarmKit,
  libwebrtc) in ~14 s + 9 s. 3,394,690 -> 3,623,312 blocks.
- **Cold opens** (content, s; fresh boot, then killed; openm.sh, 2 rounds):
| App | before | after |
|---|---|---|
| Settings | 7.15 (first after boot) / 3.39 | 3.79 / 3.46 |
| x86ui | 1.90 / 1.81 | 1.78 / 1.64 |
| Cadence | 2.13 / 2.59 | 2.37 / 2.07 |
  The first Settings open after boot went 7.15 -> 3.79 s (one sample each). The rest is within noise to about -10%:
  the translation that went away was a smaller part of the open than the share suggested.
- perfopen (Preferences cold, 6 s): 20.1G cycles (Speed 28 after: 23.6G), L1i misses 4.27G (4.8G), iTLB walks 5.77M.

## 3. Native ASTC decode of the SplashBoard snapshot (default on after the gates, see the end)
- libate's `at_encoder_decompress_texels` entry reads as `svc #0xfff0` (MemoryReadCode; the libate chunk was rebuilt
  so the image has it). The handler asks the encoder for its formats through libate's own getters (guest calls,
  virtual methods at encoder+80). LDR ASTC (all 14 footprints) -> RGBA8/BGRA8 with equal texel/block alpha is decoded
  by ARM's astc-encoder 5.2.0 (vendored, Apache-2.0, linux/arm64jit/third_party/astcenc, SSE4.1, a decompress-only
  context per thread and footprint). It handles the flip flag, a block row stride and the BGRA swizzle. Anything else
  emulates the replaced `sub sp, sp, #176` and runs libate.
- `X86CODE_A64_ASTC_NATIVE=verify`: 0 of 12,648,528 bytes differ (1206x2622 snapshot).
- Warm Settings tap, 5 taps each, same boot (SpringBoard restarted between configs):
| | response (s) | content (s) | SpringBoard CPU per tap |
|---|---|---|---|
| libate (=0) | 0.448-0.499, mean 0.470 | 0.739-0.783, mean 0.762 | 540-570 ms |
| native | 0.430-0.466, mean 0.440 | 0.744-0.761, mean 0.750 | 520-530 ms |
  (First native run: response mean 0.436, content 0.746-0.894: noisy.) The LZFSE part (libcompression) is not native:
  not done.

## 4. Hot/cold layout: measured, not implemented
Inside each chunk, blocks are already laid out hottest first (hot.rs in_ranges orders by profile count). A global
hot/cold split needs two chunks per library. The builder follows successors (PretranslateCollect), so a cold chunk would
re-translate hot blocks: duplicate descriptors across chunks, which the assembler does not handle. That did not fit the
time box. Counters now: Preferences cold 20.1G cycles / 53.7G instructions, L1i misses 4.27G (79 per 1k
instructions), iTLB walks 5.77M; SpringBoard idle 5 s: 176M cycles, 57.7M L1i misses. Next: chunk split with a
"hot" sub-slot region and a successor filter in the builder (only blocks of the same sub-chunk).

## 5. Maps route: the next step done
New X86CODE_A64_AGOUT=<attr|1> (27.2 addresses, with LIVE_ONLY=Maps): SVC probes at AGGraphSetOutputValue /
AGGraphGetOutputValue record (current attribute from the UpdateStack's top frame, value, type, lr, tid) into a
64K ring; the abort dump (ABORT_TSD) prints the failing attribute's events. Result (s31m32, 1 of 3 runs aborted):
the failing attribute 0x3fe0d8 (frame flags 5) **did call AGGraphSetOutputValue**: the newest event of the last 65536,
from `DynamicContainerInfo<DynamicLayoutViewAdaptor>.updateValue()` +3324 (value on the stack, type 0x222b4cb08), right
before "failed to set an initial value". So the value is set and then lost or rejected inside AG's set path. Next:
probe the return of AGGraphSetOutputValue and read the node's value/flags word (did the store land; a write lost to
the data::table remap, or a type mismatch?).

## Small fixes
- Gate helpers: `pid_of` / new `pids_of` match only this phone's processes (the process environment names the
  device's data dir). Used by gate.sh, openm.sh, perfopen.sh, tapm.sh (they matched by name across phones).
- Keyboard: the first host key attaches simhid's keyboard service, which puts UIKit in hardware-keyboard mode and
  restarts the field's keyboard session. The next keys came 300 ms later and were lost. simhid now waits
  SIMHID_KB_SETTLE_MS (default 1500) after that first attach. The client's later lines queue in the socket in order,
  nothing is dropped. The gate logs whether "wifi" reads back whole (info line).
- a64-penv is cleared (empty file). It is shared by every phone using this libarm64jit.

## Tools
tests/arm64/gate/nonimage.sh (perf split of a cold open), openm.sh opens x86ui / Cadence by bundle id,
X86CODE_A64_LIVE_BINS, X86CODE_A64_AGOUT, x86code-aot `train_until` config field (training .bins newer than it are
ignored), $SCRATCH/s31/tools/cd2.py.

## Gate
s31g1, s31g2, s31g3 (cold boots, default config incl. native ASTC on and the simhid keyboard settle): **0 failed
each, 3 consecutive clean**: home t+26 / 7 / 7 s, every app check HIT, "first keys: wifi typed whole" in all three.
So native ASTC decode and the keyboard settle are default. Still opt-in (not gated as defaults): X86CODE_A64_AOT_LIVE,
X86SIM_APP_AOT, X86CODE_A64_LIVE_BINS, X86CODE_A64_AGOUT. The image changes (app chunks, .live blocks, libate's svc
entry) are data and are in all three gate boots.
tests/apps/run.sh var/device-a64-ios272 --no-build: **PASS** (x86data 9/9, x86net 5/5, x86ui 7/7;
$SCRATCH/apps/run-20261101-233333). Note: a reinstall gets a new container UUID, so its placement keys change: run
placeapp again (the X86SIM_APP_AOT hook does).
The phone is left booted at home (page 1) in the default config; a64-penv is empty.
