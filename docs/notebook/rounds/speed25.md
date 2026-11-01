# Speed 25 (2026-11-01): app-open speed on iOS 27.2

Phone: var/device-a64-ios272 (iPhone 17, 27.2 24B8117), image $SCRATCH/jc-24B8117.
Method (same every time): tests/arm64/gate/openm.sh: tap the home-screen icon, frame ring captured (simrecord, scale
4), times from the simhid touch-down: response (first changed frame), black launch gap, content (settled screen).
Cold = the app's process killed first (SIGKILL), open from home; warm = home, tap again.

## Baseline (06:30, the image as Speed 24 left it)
| App | cold content | black gap | warm content |
|---|---|---|---|
| Settings | 5.53 s | 0.60-4.80 s | 0.77 s |
| Messages | 4.30 s | 0.58-4.02 s | 0.73 s |
| Health | 9.93 s | 0.55-8.03 s | (prompt) |
| Wallet | 6.61 s | 0.62-6.24 s | 0.75 s |
| Maps | 5.16 s | 0.63-4.92 s | 0.61 s |

## Where a cold open goes (Settings, perf -a 8 s + guest sampler)
- Preferences main thread 4.5 CPU-s in the 8 s window; a second thread 3.6 CPU-s. Dispatcher (GetBlock +
  LookupBlockThunk + FastDispatch probe) ~10% of the main thread; live translation tiny; code from the AOT image 43%,
  live-translated (non-image) code 31%.
- Guest sampler (X86CODE_A64_GSAMPLE=Preferences, all threads): in the first 10 s, 371 of 550 running samples (67%)
  in one libswiftCore loop: _searchConformancesByMangledTypeName (swift_getTypeByMangledName's fallback when a name
  misses the type-metadata records) walks every protocol-conformance record of every image (627 sections, 345,900
  records, 612 sections in the shared cache) and calls _contextDescriptorMatchesMangling per record. dyld's Swift
  tables are active (version 1, cache range known) but this walk doesn't use them.
- Found on the way: Speed 7's native conformance step (swiftconf.cpp) used 27.0's addresses, so on 27.1/27.2 it was
  never applied (the code is at 0x1c15f6ae0 in 27.2).

## Levers, before -> after (cold content, frame-timed; 2 runs where shown)
| Step | Settings | Messages | Health | Wallet | Maps | warm (resp / content) |
|---|---|---|---|---|---|---|
| Baseline | 5.53 | 4.30 | 9.93 | 6.61 | 5.16 | 0.44-0.48 / 0.73-0.83 |
| A: Swift steps (walk native, Speed 7 step ported), no index | 5.49 | 3.88 | 8.84 | 5.91 | 5.24 | same |
| + index, dyld_sim placed, retrained image (codegen 0x17) | 4.72 / 4.42 | 3.10 / 3.03 | 6.89 / 5.95 | 4.38 / 3.75 | 4.28 / 3.39 | same |
| + RSB (codegen 0x18) | 4.57 / 4.15 | 2.64 / 2.65 | 5.95 / 5.04 | 3.38 / 3.64 | 2.86 / (5.57) | 0.37 / 0.66-0.87 |
Run-to-run noise is large (another phone, "QA", ran on the host for part of the round); the first open after a boot is
slowest.

## What was done
1. **Swift type lookup by name (swiftscan.cpp).** _searchConformancesByMangledTypeName: the loop head is read as an
   svc; the handler walks natively and, for a nominal node with an Identifier name, uses an index (sorted (FNV(name),
   position) pairs over every class/struct/enum descriptor without import info, extended as sections are appended);
   only records the matcher must judge go to the guest. Node layout from swift/Demangling/Demangle.h; this runtime's
   kinds Class 26 / Identifier 104 (6.2 + 1). Every lookup seen in Settings/Health/Wallet was a Class node whose name no
   descriptor carries (a full miss). Guest running samples in Settings' first 10 s: 626 -> 185; the walk 67% -> ~1%.
   Code found in libswiftCore by pattern, read from the cache files (guest cache pages are mapped on demand; the
   image builder maps the header page without access). X86CODE_A64_SWIFT_TYPE_NATIVE/_INDEX/_FILTER=0, _DEBUG=1.
2. **Speed 7's conformance step on 27.1/27.2**: it had 27.0's constants; now found by pattern (27.2: 0x1c15f6ae0).
3. **dyld_sim placed** at its slot (0x2bd9a0000 on 27.2): map_macho skipped placement for dyld_sim, so its prepare
   code (~1.6 s per app process) was translated live in every process. The builder maps it writable and applies the
   same matchesPath patch.
4. **Training** (tests/arm64/train.sh): the gate pass + Messages, News, Safari, Settings pages, switcher, with
   X86CODE_A64_AOT_AUTOSAVE=1 (without it processes that load the image write no profile: the first training pass
   recorded nothing). Image 1.88M -> 3.06M blocks, 637 -> 1029 MB.
5. **RSB on** under shared translations: pops compare A64JitState::rsb_gen with the shared invalidate_gen (pointer in
   the JIT state); a newer generation drops the thread's entries and goes to FastDispatch. The svc block's pop is now
   predicted too (svc pushes svc+4), so this also gives what FUSE_SVC/SKIP_RET aimed at (lever 2 not retested as a
   separate flag; FUSE_SVC is code-shaping and still off).
6. **RSB persistence bug** (found by the gate's launchd_sim deaths): PushRSB `mov rcx, imm64` sites were not
   persisted; a saved slab could hold a patched block pointer under a ReturnFromRun reloc, or point into int3 filler.
   launchd_sim (own .bin) died of SIGTRAP minutes after boot. Sites are now saved unlinked and as links (codegen 0x19).

## Fixed on the way
- **News / Safari closing ~11 s after opening**: JavaScriptCore maps JIT thunks execute-only; on a PKU CPU Linux
  makes that a protection-key page and our translator's read faulted (SEGV_PKUERR, "fatal native fault"). The fault
  handler makes the page readable. News opens ("Welcome to Apple News", offline) and stays; Safari loads apple.com.
- **Messages conversation spun forever**: StatusKitAgent was not in the job set; IMCore's per-item StatusKit sync XPC
  failed and repeated. Added (on demand).
- **launchd #IfFeatureFlagEnabled conditionals** resolved by mksimdevice with the runtime's flags (27.2's keyboard
  arbiter service now in InputUI as UIKit expects). Not enough for the keyboard, see below.

## Open
- **Keyboard on 27.2 (predates this round: train1 ran on the old binaries and failed the same way).** Text fields
  don't take focus (Settings / Messages search, iMessage field). SpringBoard: "[KeyboardFocus] invalid pid for scene
  sceneID:com.apple.Preferences-default, returning nil target"; InputUI: "elapsedCPUTimeForFrontBoard task_info failed
  with 0x4". With the arbiter in SpringBoard (flag overridden off) it fails the same way. Next: why the scene's
  process handle has no valid pid for KeyboardFocus (RunningBoard/FrontBoard process identity).
- **Siri on 27.2**: the gate's home-button hold starts no Siri process (also before this round).
- Spotlight ~100% CPU / 2.4-3.7 GB, Settings search typing spin (launcher QA report): not started.
- Lever 4b (GETBLOCK_FD probe): not done.

## Zero-crash gate (27.2 phone, final binaries, codegen 0x19): NOT fully met
z1-z3 (08:40, 08:45, 08:50), 3 consecutive cold boots: **0 CRASH lines**; home, Settings cold/warm, General > About,
Calendar, Photos (cold; warm shows the "What's New in Photos" sheet, which the gate's regex doesn't accept), Maps,
Files (opens; "Content Unavailable ... unknown error" in Recents), Health, Wallet all open; home swipe-up and the app
switcher work. **Keyboard (TIMEOUT) and Siri (0 processes) fail in all three**: both also failed before any Speed 25
change (train1, old binaries). The default device's 27.0 runtime is still missing, so the gate ran on 27.2 only.
State left: 27.2 phone booted at home, default config.
