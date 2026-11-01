# Speed 26 (2026-11-01): keyboard, network, Siri, Files on iOS 27.2

Phone: var/device-a64-ios272 (iPhone 17, 27.2 24B8117), image $SCRATCH/jc-24B8117 (codegen 0x19, 3.06M blocks, kept).
Method: reboots with per-process os_log (`/private/var/tmp/x86code-stderr-progs` in the prefix), arm64 diag shims
(`DIAG_ARCH=arm64 build/mac-build-diag.sh <name>`, X86CODE_INSERT), strace of the Linux processes, guest sampler.

## 1. Keyboard (fixed) and the Settings-search spin (fixed)
- "[KeyboardFocus] invalid pid for scene" was a red herring: SpringBoard builds a focus target from
  `scene.clientHandle.processHandle.pid` when the scene is created, before the app's spawn completes. A generic objc
  tracer in SpringBoard (host/simshim/diag/kbdiag.m: every method of the focus classes through one arm64 trampoline that
  looks the original up by (class, SEL)) showed the real target built at `didAddExternalForegroundApplicationSceneHandle`
  and the deferring rule set (`deferFromSBWindowScene:toTarget:`): focus was fine.
- On-screen keys typed; hardware keys (simhid) didn't, and the app spun. Guest sampler on Preferences (73% CPU from the
  moment the software keyboard showed): `AFDictationConnection dictationIsAvailableForLanguage:` -> error ->
  "availability changed" notification -> `UISystemKeyboardDockController updateDockItemsVisibility` -> ask again, the
  same loop as Speed 12/13. Speed 13 had cut assistantd to `com.apple.assistant.dictation` only; on 27.2 the check also
  needs `com.apple.assistant.settings` (failed lookups from Preferences and SpringBoard).
- Fix: assistantd is Apple's plist (all ~30 MachServices; `X86SIM_ASSISTANTD=dictation` keeps the old slim one for
  experiments). Settings search: "wifi" typed from the host keyboard, Preferences idle afterwards (1 tick in 10 s).
  Gate keyboard step (Spotlight): "wifi" typed in z1, z2.

## 2. Siri (invokes; no visible UI yet)
- iPhone 17 has no home button: the gate's home-button hold did nothing. The side button (lock, 2.5 s) starts Siri.
- With full assistantd SpringBoard showed "Siri Not Available - Data for using Siri is downloading": assistantd found no
  UAF asset set (com.apple.siri.understanding: errno 2). A Mac's iOS 27 device downloads them (Understanding 738 MB, TTS
  93 MB, ASR 38 MB) with its own mobileassetd. Added mobileassetd + assetsubscriptiond (on demand), and corespeechd,
  sirittsd, siriinferenced, siriknowledged, siriactionsd (a Mac has them on demand; without corespeechd recording failed
  at once, kAFAssistantErrorDomain 221).
- Now: the side button starts the `Siri` process and SpringBoard's Siri presentation; the request ends without a visible
  UI (assistantd: audio provider / accounts / Siri server connection). Assets: the ASR set (39 MB) is installed; the
  others download as mobileassetd schedules them.

## Network on arm64 (found on the way; needed by Siri assets, Safari, News)
The arm64 phone had no working network at all:
1. **No DNS**: boot-a64 ran no mDNSResponder stand-in (`/var/run/mDNSResponder` missing, -1003). It now starts its own
   linux/mdnsd server per boot (`$ST/mDNSResponder`, X86CODE_MDNS=0: none) and links it into the container.
2. **Offline (-1009)**: mksimdevice skips com.x86code.netconfig when host/netconfig/arm64/netconfig is missing, so the
   host network state never reached configd_sim. Added the arm64 build (build/mac-build-arm64-shims.sh netconfig).
3. **No TCP socket**: iOS 27's Network.framework makes flow sockets with `socket_delegate` (450) and connects with
   `connectx` (447); Darling had neither. Implemented in Darling's syscall table (also `disconnectx`).
4. **Safari: WebKit helpers exited at once** ("Unable to find entry point in WebKit.framework with name:
   WebContentServiceInitializer"). runtimes/<rt>/RuntimeRoot is a symlink into extracted/...; dyld_sim strips only the
   realpath of the simulator root, so `_dyld_shared_cache_contains_path("<symlinked root>/System/Library/Frameworks/
   WebKit.framework/WebKit")` was false (diag shim cachediag.m: 0 for the symlinked forms, 1 for the realpath forms) and
   CFBundle found no executable for cache-only frameworks. mksimdevice now uses the realpath root (DYLD_ROOT_PATH etc.;
   SimulatorRuntimeRoot already was). The first boot after the change re-registers apps (LaunchServices rebuild,
   progress-bar boot, then the known black-home first boot: restart once).
5. **Safari aborted** on `mrs x8, CNTVCT_EL0` (JavaScriptCore's WTF::UnbarrieredMonotonicTime::calibrate) and WebContent
   on `fjcvtzs` (FEAT_JSCVT). dynarmic now reads CNTVCT/CNTPCTSS/CNTVCTSS like CNTPCT; the fallback executes them (image
   blocks of the same codegen still exit there: no codegen bump, the trained image is kept) and FJCVTZS (JS ToInt32,
   Z flag when exact).
6. **Downloads never installed**: iOS 27's Foundation uses `/.nofollow/<path>` (resolve without symlinks); Darling's
   vchroot took it literally (`<prefix>/.nofollow/Volumes/...`), so nsurlsessiond's finished downloads never reached
   mobileassetd's staging ("Info plist does not exist", "Failed to decrypt asset content"). The prefix is now stripped.
   After it: com_apple_MobileAsset_UAF_Siri_UnderstandingASRHammer (39 MB) installed.
Result: Safari loads en.wikipedia.org pages. Still open: some flows fail at once with ENETDOWN inside Network.framework
(no failing syscall; the first load after a Networking process starts, mobileassetd's catalog fetches right after boot)
and mobileassetd then backs off.

## 3. Spotlight "runaway" -> a crash when typing (root cause found; not fixed)
- Not reproduced as a runaway this round (idle at 1.0 GB after search; the session-start 58% / 1.5 GB was on the old
  binaries, with the dictation loop above).
- Typing in Spotlight search crashes Spotlight (SIGABRT, relaunched): `+[SPSearchEntity searchEntityWithPhotosSearchString:
  fromSuggestion:]` -> `-[_CSSuggestionToken initWithUserString:...]` builds `@{TOKEN: nil, ...}`
  (host/simshim/diag/nildictdiag.m: TOKEN nil, TEXT "wifi", the Photos query). The token comes from the query parser,
  whose UAF asset `com.apple.search.queryparser` (SearchQueryUnderstanding) isn't installed: "U2 Model not found via UAF
  or system path", "Failed to request sandbox extension for com.apple.search.queryparser". Not the AOT image (JIT-only:
  same), not the RSB (off: same), not Metal/Espresso (software rendering: same). A Mac's iOS 27 devices that have run have
  this asset (downloaded). It arrives through the same mobileassetd path as Siri's (network item above).

## 4. Files (fixed)
"Content Unavailable ... unknown error" in Recents: Files' `com.apple.spotlight.SearchAgent` lookups failed; searchd
(SearchAgent / IndexAgent / IndexDelegateAgent / MobileAsset.SpotlightResources) wasn't a job. Added searchd and
corespotlightservice (on demand); searchd then exited each second: its index-state rename used `renameatx_np` flags
Darling rejected with ENOTSUP (45). RENAME_SECLUDE / NOFOLLOW_ANY / RESOLVE_BENEATH accepted. Recents now shows "No
Recents - Recently opened documents will appear here."

## 5. Photos "What's New" (gate)
The sheet is recognised, logged and dismissed (Continue; OCR misses the white-on-blue button, so its place is tapped),
then the notification prompt is answered (tests/arm64/gate/lib.sh photos_whatsnew).

## Gate changes
- Siri: side button; counts Siri's UI processes (Siri, Campo, SiriViewService), not assistantd (always running now).
- `FRESH` rejects the previous boot's last frame (STALEFRAME, its ring frame number): z1's home "hit" was that frame.
- Stall = no screen *and* no guest-CPU change (the boot logo is static while lsd/datamigrator work).

## Not done
- 6 (GETBLOCK_FD probe + re-measure): not started.
- The intermittent ENETDOWN in Network.framework, the queryparser asset (Spotlight crash), Siri's UI.
