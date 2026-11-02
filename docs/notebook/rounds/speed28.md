# Speed 28 (2026-11-01): dyld_sim loader scans, huge pages (blocked by host), JSC stale code, gate

Phone: var/device-a64-ios272 (iPhone 17, 27.2 24B8117), image $SCRATCH/jc-24B8117 (only dyld_sim's chunk rebuilt:
3394342 -> 3394690 blocks). Method: logs first, openm.sh (frame-timed), perf stat/record on Preferences, guest sampler
(X86CODE_A64_GSAMPLE), dyld_sim disassembly, ipsw a2s. Haiku subagent for the env-propagation lookup.

## 0. Baseline, and why Speed 27's cold numbers looked worse than Speed 25's
Cold content, frame-timed from touch-down (s). "killed" = openm.sh default (app SIGKILLed, then opened from home, 2 runs);
"fresh" = the first open after a fresh boot (25 s after the boot script returns, 2 boots).
| App | killed (start) | fresh (start) | warm |
|---|---|---|---|
| Settings | 5.12 / 4.98 | 5.33 / 5.38 | 0.80-0.89 |
| Messages | 3.29 / 3.22 | - | 0.75 |
| Health | 6.95 / 6.88 | 8.95 / 9.65 | 0.76-0.95 |
| Wallet | 3.92 / 3.98 | - | 0.76-0.79 |
| Maps | 6.39 / 5.91 | 7.33 / 7.23 | 0.84-0.95 |
- Not a regression of the Speed 26/27 code: the two methods measure different things. A first open after boot also
  spawns the app's on-demand daemons, each paying its own dyld_sim prepare: Health starts healthd, healthappd,
  CoreSpotlightService, SetStoreUpdateService, DayStreamProcessorService, HistoricalAnalyzerService (healthappd ~8 s
  after the tap); Maps starts navd and AppIntentsLiveEntityService. "killed" keeps them alive. Speed 25's Maps 2.9 s was
  the bare globe (NanoRegistry deadlock, fixed in Speed 27); Maps now draws tiles before "content".
- The killed-first Settings 5.0 s vs Speed 25's 4.2-4.6 s is within host-state noise here: the host had 1 GB free,
  29 GB swapped and /tmp holding 9.3 GB; Speed 25's notes already tie dyld prepare spread to page-cache misses. The M1
  CPU identity could not be A/B'd without a libsystem_kernel rebuild (no runtime switch); not done.
- perf stat, Preferences cold (6 s from process start): 28.9G cycles, 97G instructions, 349M L1-iTLB misses hitting
  L2, 5.48M iTLB walks (L2 miss), 5.0G L1i misses. SpringBoard idle 5 s: 0.50M walks, 56M L1i misses.
- Boot: boot script returns at ~26 s; gate clock: home at t+7 s after SpringBoard starts.

## 1. Huge-page AOT image: implemented, BLOCKED by the host (no 2 MB pages available)
- dynarmic LoadPersistentCache maps the shared relocated .map from a tmpfs copy (X86CODE_A64_AOT_HUGE_DIR, default
  /tmp/x86code-a64-img: the container's /dev/shm is noexec) filled through a MADV_HUGEPAGE MAP_SHARED mapping (+
  MADV_COLLAPSE), each process maps it MAP_PRIVATE RWX + MADV_HUGEPAGE (code region base is 2 MB aligned, so VA and file
  offset agree mod 2 MB). Standalone probe: a private RWX mapping of a huge shmem folio does PMD-map (ShmemPmdMapped).
- On this host the fill gets 1 of 32 huge folios: /proc/buddyinfo has zero free order-9 blocks, compact_memory (one
  manual run) frees none (compact_fail 1.4M of 1.8M stalls), every mTHP size is "never" (host setting, not changed).
- So the copy is kept only when >= 50% of it is PMD-mapped; otherwise it is dropped and a `.small` marker makes the
  rest of the boot map the .map file as before (boot-a64 clears markers each boot). No RAM cost when it can't help.
  X86CODE_A64_AOT_HUGE=0 turns it off. **Accept criterion (walks /5) not measurable here**: needs a host with free 2 MB
  pages (fresh boot, or less memory pressure).

## 2. dyld_sim loaded-loader scans native (new lever, the round's main win)
- Guest sampler on a cold Preferences: ~45% of running samples in the first 10 s were in dyld_sim
  JustInTimeLoader::matchesPath, called from the loops in JustInTimeLoader::loadDependents' block (0x20498) and
  Loader::getLoader's block (0x2483c): every lookup walks all ~900 loaded loaders (O(images^2) per process; Speed 6
  only made the install-name part cheaper).
- dyldsim.cpp: both loop heads read as `svc #0xfffb/#0xfffa` (MemoryReadCode); the handler walks the vector natively
  (path strcmp, LC_ID_DYLIB name for dylibs), same order and exits; PseudoDylib loaders, non-JustInTimeLoaders and an
  invalidated vector resume the guest's own instructions. Loops found by code words (any 27.x build). dyld_sim's image
  chunk rebuilt (x86code-aot update after moving the chunk aside) so the old loop blocks left the image.
  X86CODE_A64_DYLD_SCAN=0 turns it off.
- dyld_sim prepare: Preferences 1.65 s -> 0.73 s, Health 1.55 -> 0.90 s, healthd 0.13 s, navd 0.20 s.
| App | killed before | killed after | fresh before | fresh after |
|---|---|---|---|---|
| Settings | 5.05 | 3.33 / 3.71 | 5.35 | 4.15 |
| Messages | 3.25 | 2.32 / 2.07 | - | - |
| Health | 6.9 | 5.23 / 5.30 | 9.3 | 7.43 |
| Wallet | 3.95 | 2.87 / 2.86 | - | - |
| Maps | 6.15 | 5.09 / 5.10 | 7.28 | 5.94 |
  Warm unchanged (0.75-0.85 s). perf, Preferences cold: 28.9G -> 23.6G cycles; iTLB walks unchanged (5.57M), L1i
  misses 5.0G -> 4.8G. Live-translated (non-image) code: 18% -> 9% of Preferences samples.

## 3. Non-image code / hot-cold layout: not done
- After 2, Preferences cold (perf, 6 s): image 50%, libarm64jit 31% (swiftconf_step 9.4%, GetBlock 5.6%, FastDispatch
  probe 2.9%, LookupBlockThunk 2.4%), live JIT 9%. The remaining non-image share is mostly the svc blocks of the native
  Swift steps (never persisted, by design) and app code.
- swiftconf: 9.4M guest round trips per cold Settings open (40M records skipped natively). Class queries get no native
  filter; the match path compares [sp+0xb0] with the record's type descriptor, which does not fit the existing filter's
  "metadata at [sp+0xb0]" reading. Settle that before extending the filter (Speed 29 candidate: ~9% of the cold open).

## 4. WebContent on en.wikipedia.org: one root cause FIXED, a second crash narrowed (gate still fails)
- **Fixed: JSC's JIT code ran stale translations.** The simulator's libsystem_platform re-exports the host's, so
  sys_icache_invalidate / sys_cache_control went to Darling's x86_64 libplatform (no-ops): no guest IC IVAU ever ran in
  WebContent (ICLOG: 0), so code JSC rewrote at a reused address kept its old translation (the random NULL reads in JIT
  code; with the AOT image JSC is fast enough to tier up and recycle). handlers.cpp: both now invalidate the range
  in the JIT (thousands per page load). X86CODE_A64_ICACHE_NATIVE=0 = old behaviour.
- **Open: a deterministic crash in WebCore::DOMWrapperWorld::clearWrappers()+852** (write to 0x80000008: a WeakImpl
  pointer 0x80000000 in the wrapper map), from Safari's Reader detection on a timer
  (ReaderAvailabilityController::extractPageMetadataAndTextNow -> clearArticleFinderIfPossible ->
  -[WKWebProcessPlugInScriptWorld clearWrappers]). It happens with the AOT image, without it (X86CODE_A64_LIVE_ONLY), and
  with JSC's JIT off (JSC_useJIT=false: no JIT region at all): not JIT code, not the image, not FTL. Next: watch who
  writes the map entry (guest watchpoint on the isolated world's m_wrappers table), check WTF HashTable rehash paths
  through the native memcpy/memmove (>= 256 B) and 32/64-bit atomics on that table.
- Debug knobs added: X86CODE_A64_AOT_EXCLUDE=procs (off the image), X86CODE_A64_LIVE_ONLY=procs (no image, no .bin),
  X86CODE_A64_GENV_<NAME>=v (guest env NAME=v, e.g. JSC options for launchd jobs), X86CODE_A64_INVAL_DEBUG=<prefix>
  (blocks found per range invalidation). Native fault dumps now print all 31 registers and 48 words of guest code from
  the block PC.

## 5-6. ASTC/LZFSE decode, mach_msg measurement, scope "More": not done this round.

## 7. Maps route (AttributeGraph): not worked on this round.

## Gate
s28a1, s28g1, s28g2 (cold boots): each 1 failed check, the WebContent clearWrappers crash; every other check passes
(home t+7 s, Settings cold/warm, About, Calendar, Photos, Maps + tiles, keyboard/Spotlight, Siri processes, Files,
Health, Wallet, Messages, News, swipe-up, switcher). **Zero-crash gate NOT met** (the pre-existing WebContent crash);
the Speed 28 defaults (native dyld scans, icache invalidation, huge-page attempt) were committed and pushed without a
clean gate.

## Tools / files
linux/arm64jit: dyldsim.cpp (loader scans), handlers.cpp (sys_icache_invalidate / sys_cache_control), jitcache.cpp
(AOT_EXCLUDE, LIVE_ONLY), host.cpp (GENV, fault dump), jit.cpp, a64.h; external/dynarmic a64_emit_x64.cpp (huge-page
copy, invalidation debug); cli/libexec/arm64/boot-a64.sh (clears .small markers); tests/arm64/gate/perfopen.sh (perf
stat on SpringBoard idle + Preferences cold).
