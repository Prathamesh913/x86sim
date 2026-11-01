# Speed 27 (2026-11-01): Spotlight, boot time, network, Maps, Siri, gate on iOS 27.2

Phone: var/device-a64-ios272 (iPhone 17, 27.2 24B8117), image $SCRATCH/jc-24B8117 (codegen unchanged; retrained:
3.06M -> 3.39M blocks). Method: logs first (process list, per-process JIT logs in $PREFIX/private/var/tmp/x86code-a64,
launchd_sim log, job logs, os_log via x86code-stderr-progs or the arm64 oslogdiag shim), strace of the Linux processes,
arm64 diag shims (kevdiag, errdiag +enoent, agdiag), guest sampler (X86CODE_A64_GSAMPLE[_ALL], _OUT), ipsw against the
27.2 dyld cache, small arm64 probes (tests/arm64/netprobe.c; scratch: cpuprobe, replprobe, clockprobe). Mac mini used
read-only (its iOS 27 simulators' data, `sysctl`, /System/Library/GeoServicesData). Log tallies and symbolization were
done by Haiku subagents.

## 1. Spotlight crash when typing: FIXED (root cause was not the asset)
- The SearchQueryUnderstanding asset (com.apple.search.queryparser): a Mac's iOS 27 devices have it downloaded
  (data/private/var/MobileAsset/AssetsV2/com_apple_MobileAsset_UAF_SearchQueryUnderstanding, 106 MB). Ours now
  downloads it too (106 MB, after the network fix, item 3), as on the Mac.
- With the asset, Spotlight still crashed. The nil TOKEN is `CSGetLocalizedString(@"PHOTO_TOKEN")` (found by
  disassembling -[_CSSuggestionToken initWithUserString:...] at the crash; the object comes from a dispatch_once
  static). CoreSpotlight reads its strings with `[NSBundle bundleWithPath:@"/System/Library/Frameworks/
  CoreSpotlight.framework"]`: an absolute host path. On a Mac that is macOS's CoreSpotlight.framework; here it was the
  Darling install root's link into the iOS 26.5 runtime, which no longer exists (data loss): every CoreSpotlight
  string was nil. **Fix**: boot-a64.sh links the booted runtime's CoreSpotlight.framework (real path) into the
  prefix's upper layer at every boot (the x86_64 boot's run-launchd-sim.sh did the same in Spike 3.12).
- Also fixed on the way, so the query parser works as on a Mac:
  - Darling fcntl F_PREALLOCATE (was EINVAL): Core ML's BNNS graph compiler failed ("failed to preallocate file"); the
    U2 model now compiles once and loads from the E5 bundle cache. Plus F_RDADVISE, F_RDAHEAD, F_NOCACHE,
    F_BARRIERFSYNC, F_PUNCHHOLE, F_GETPATH_NOFIRMLINK, F_SETCONFINED/F_GETCONFINED (CoreEmoji, CoreNLP, SQLite).
  - assistant_cdmd job (com.apple.assistant.cdm, Apple's plist, on demand): the parser's embedding service.
  - arm64 processes see an Apple-silicon CPU (hw.cputype ARM64, hw.cpusubtype 2, hw.machine/uname "arm64",
    hw.cpufamily M1, hw.optional.arm64 and the hw.optional.arm.FEAT_* keys limited to what the JIT executes); they saw
    the Linux host's x86_64. ("Compile for platform intel" in E5 is normal: a Mac's simulator E5 bundles are
    intel.bundle too.)
- Result: typing "wifi photos" in Spotlight shows Suggestions (Safari "wifi photos") and Search in Apps; no crash
  (nildictdiag: no nil insert). Gate keyboard check types two words and checks Spotlight's JIT log for a crash.

## 2. Boot time: ~73-82 s -> ~19-40 s (FIXED)
- Not the Siri/search daemons: lsd rebuilt the LaunchServices database at every boot (98 CPU-s; "rebuildReasonFlags 33",
  `_LSSchemaConfigureForStore ... WrongSimulatorHash`, totalRebuildTime 44.7 s) because it could never save the
  rebuilt store: "Failed atomic move of temporary CSStore file ... doesn't exist".
- `-[NSFileManager replaceItemAtURL:...]` (CoreServicesInternal URLReplaceObject) resolves the item's parent directory
  by ID with fsgetpath(); Darling's getattrlist remembered only the object's own ID for fsgetpath, never the parent ID
  it hands out (ATTR_CMN_PARENTID/PAROBJID) -> ENOENT. Reproduced with an arm64 probe (replaceItem failed the same
  way); errdiag (+enoent) showed `fsgetpath(..., parent inode) -> errno 2`. **Fix**: getattrlist also remembers the
  parent's path for the parent ID. Every atomic replace (NSFileManager) works now.
- Measured (bootprof: boot-a64 start -> home on screen): 73-82 s before; after, with the store saved: 16-42 s across
  ~25 boots this round (typical 19-30 s; the higher ones were boots right after an image rebuild or with diag shims).
  The gate's own clock (from container stop) gave 13 s and 40 s.

## 3. Network: first-connection ENETDOWN FIXED
- What Network.framework saw: path satisfied (enp15s0) every time; connect() returned EINPROGRESS (strace); yet the flow
  failed "Network is down" ~10-40 ms later. kevdiag (kevent results per socket): EVFILT_READ fired EV_EOF on the new
  TCP socket within ms of its EV_ADD.
- Two Darling bugs:
  1. sostate's TCP_INFO read raced connect() on another thread: Linux's tcp_get_info reads the state without the socket
     lock, so a read during connect saw CLOSE with the SYN already counted -> classified "disconnected", and libkqueue
     kept the new socket's hang-up as EOF. **Fix**: linux_tcp_info re-reads once in that case (connect sets SYN_SENT
     before it sends, so the re-read is consistent).
  2. libkqueue: close() issued EV_DELETE without EV_ENABLE, which the x86Code defer-delete rule (for programs'
     deletes) refused on a disabled EV_DISPATCH knote: the knote survived close with its duplicate of the old file;
     **fix**: close drops knotes unconditionally (XNU's knote_fdclose), and EV_ADD replaces a knote whose duplicate is
     another file.
- Result: mobileassetd 0 "Network is down" (was 3-5 per boot), all catalogs and assets download (SearchQuery 106 MB).
- prefix-heal.sh now also installs libsystem_c (Darling's libkqueue) and libkqueue_sim.

## 4. Maps: tiles, search, place card, directions (mostly FIXED)
- Bare globe grid, no tiles, no input: Maps' main thread was deadlocked inside +[NRPairedDeviceRegistry
  sharedInstance] (guest sampler + symbolization): NanoRegistry's sync XPC query failed its lookup (no nanoregistryd),
  NSXPCConnection ran the error handler inline, and that dispatch_sync'd onto the queue the thread held. A Mac's
  simulator runs nanoregistryd. **Fix**: nanoregistryd job (Apple's plist, on demand).
- geod's seed manifest /System/Library/GeoServicesData/ResourceManifest.json is a Mac host path (18 MB on the Mac, not
  in the runtime). cli/libexec/arm64/fetch-macos-host.sh fetches it from the Mac into runtimes/macos-host (gitignored);
  boot-a64.sh links it into the prefix.
- Swift type index (Speed 25) dropped indirect conformance records whose slot wasn't bound when the section was
  indexed; they now go to the matcher (resolved at lookup).
- geod throttling (Speed 24): gone (no throttled requests; only "Could not add listener" config faults).
- Result: the globe with land, water, labels; search "Eiffel Tower" -> results; place card (3D Paris, Eiffel Tower
  model, hours, ratings, Plan/Download/Website/Tickets); Directions card (car/walk/transit/bike).
- **OPEN**: building the route (origin chosen) aborts Maps: AttributeGraph "precondition failure: attribute failed to
  set an initial value" (varying SwiftUI attributes, e.g. DynamicContainerInfo<DynamicLayoutViewAdaptor>). Ruled out:
  deadline cancellation (agdiag: no deadline set, no cancel calls), the AOT image (JIT-only crashes too), a multi-core
  race (pinned to one CPU: 4/4 crash), either native Swift step alone (about 50% with each off). Default config: 4/4.
  Not fixed this round; tests/arm64/gate/mapsroute.sh reproduces it (not in the gate).
- Pan/zoom: map gestures not separately verified beyond the place card's camera move.

## 5. Siri UI: NOT fixed (blocker found)
- Fresh trace: the endpointer connects now (corespeechd's com.apple.corespeech.corespeechd.endpointer.service; the old
  4099s predate Speed 26's corespeechd job). Recording fails for lack of an audio input device: "siriInputSource 'sino'.
  No Input available", AudioQueue -66680 finding Default-InputOutput, prepareRecordForStream -11790. SpringBoard then:
  "Attempting to use siriViewController, but one does not exist", SASPresentationManagerErrorInvalidStateTransition,
  dismissal. Accounts (accountsd), trial, biome etc. are not reachable either (not jobs).
- The audio devices are what a Mac's CoreSimulator/coreaudiod provide. host/simaudio (Spike 3.18's unfinished HAL
  stand-in) now has an arm64 build (build/mac-build-arm64-shims.sh simaudio) and boot-a64 starts its host side
  (opt-in X86CODE_AUDIO=1); inserted and running, but corespeechd's AudioQueue never reaches its interposers (no
  simaudio log line): the input device still comes from the empty HAL. Next: see how 27.2's AudioToolbox reaches the
  HAL (bind table / HALC proxy), then accountsd.
- The gate's Siri check (Siri UI processes start) passes in gates; the UI never shows.

## 6. Gate (tests/arm64/gate): logs first, new checks
- open_check takes the app's process: waits for it in the process list, fails on a crash in its JIT log or its exit,
  and reads the screen only while it lives (PROC in wait_ocr).
- New: Messages, News, Safari (en.wikipedia.org page), Maps tiles drawn (maptiles.py: water/land colours and colour
  count in the map area; the bare globe and flat cream score ~0), Spotlight typing without a Spotlight crash, home by
  swipe-up from an app, the app switcher (card label "Settings" at y~328), Health's first-run sheet handled.
- Gates this round: s27g1 2 failed (Health first-run sheet [handled since], WebContent frint32z), s27h1 1 failed:
  WebContent crashes only (6, while loading en.wikipedia.org). **Zero-crash gate NOT met** (see 7).

## 7. New: WebContent (JavaScriptCore JIT code) faults on en.wikipedia.org
- First "unimplemented instruction 1e68401f" (frint32z d31, d0, FEAT_FRINTTS, which an M1 has): implemented
  FRINT32Z/32X/64Z/64X in the fallback (fp16.cpp; mask checked against capstone: exactly those four).
- Then NULL reads in JSC JIT-generated code (guest pc in the memfd JIT region) -> "fatal native fault". Not the CPU
  identity (off: same), not the retrained image's profiles (all quarantined: same); JIT-only boots don't crash
  (slower: JSC likely never tiers up). The 14:15 training gate loaded the same live page without a crash. Open.

## 8. Re-measured (goal 6)
Cold opens = the first open after a fresh boot (openm.sh, frame-timed from touch-down):
| App | cold content | warm content |
|---|---|---|
| Settings | 5.55 s | 0.78 s |
| Messages | 3.47 s | 0.75 s |
| Health | 9.74 s | 0.73 s |
| Wallet | 4.44 s | 0.77 s |
| Maps | 7.30 s (now draws tiles) | 0.91 s |
Boot to home: 16-42 s (was 73-82 s at the start of the round). GETBLOCK_FD probe: not done.

## Tools / files
host/simshim/diag: kevdiag.c, agdiag.c, errdiag.c (+enoent, by-ID calls, writes); tests/arm64/netprobe.c;
tests/arm64/gate/{maptiles.py,mapsroute.sh}; cli/libexec/arm64/fetch-macos-host.sh; linux/arm64jit host.cpp (training
save on abort), fp16.cpp (FRINTTS), swiftscan.cpp (indirect records).
