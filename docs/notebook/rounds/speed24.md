# Speed 24 (2026-10-31 / 11-01): background apps suspend, 27.2 Maps past black, iOS version per phone — stopped by data loss

Stopped on the user's instruction after files outside our control were deleted mid-round (see "Data loss"). Nothing
was rebuilt, downloaded or booted after that.

## Commits (main, pushed)
| Commit | What |
|---|---|
| `75705c5` | RunningBoard suspends background-audio apps as on a device; opt-in os_state dump (X86CODE_STATEDUMP) |
| `4d3f91e` | 27.x: dyld_sim matchesPath + BKS UI sensor patch found per runtime; fake IOKit power ports are dead names |
| `20c43a8` | iOS version per phone: device create --ios, <device>/runtime, boot-a64 per-runtime cache; launcher + MCP |
| `9173fdc` | ARM64-PLAN + progress.json |
| `e051efb`, `0414c4e` | gate scripts in the repo (tests/arm64/gate: gate.sh, gates.sh, tapm.sh, lib.sh) |

Submodules: none touched. Left alone: untracked host/simshim/diag/lookupdiag.c, tests/arm64/*.sim.

## Goal 1: app-open "regression": not real (measurement method)
- The gates' Settings 20/10 s (and Health 11 s) are the openurl + 1-2 s fb2png/tesseract OCR poll, which has crept
  since Speed 18 (13/7, 16/8, 17-18/9, 20/10, 20-21/10-11). Speed 22 already showed 20/10 s.
- Frame-timed warm Settings tap (tapm, as Speed 21/22), 5 taps: response 0.44-0.48 s, content 0.64-0.80 s,
  SpringBoard 0.44-0.61 CPU-s. Same as Speed 21/22 (0.73-0.95 s content). No bisect needed.

## Goal 2: iOS 27.2 / 27.1 (partly done)
- **dyld_sim shortcut missing on 27.2.** libarm64jit's matchesPath patch used 27.0's offset (0x1f2f4); 27.2's
  dyld_sim has it at 0x1f378, so every process ran dyld_sim prepare slowly (20-45 s vs ~3.5 s). It is now found by its
  code (unique pattern; padding checked). 27.1's dyld_sim is byte-identical to 27.0's.
- **BKS UI sensor patch** is now BackBoardServices __TEXT + 0xb22c (same code in 27.0/27.1/27.2; the image moves).
  libobjc is identical in all three, so its patch already applied.
- **27.2 Maps black, found and partly fixed.** Guest stack sampling (X86CODE_A64_GSAMPLE) showed Maps' main thread
  waiting on geod (-[GEOUtilityService _getPathSynchronous:]) and geod's NSURLSession queue blocked in IOServiceClose:
  27.2's session invalidation closes the system-power connection that simshim fakes with a port whose receive right
  the process itself holds, so the MIG call never got a reply. The fake ports are now dead names (and
  IOServiceClose/IOObjectRelease on them succeed). Maps now draws its map background instead of black.
- **Still open on 27.2:** Maps then waits in GEOConfigGetDictionary (sync XPCSession to geod). geod logs "com.apple.Maps
  has made more than 50 over 2.0 seconds" (its request throttle). Next: why the throttle trips under the JIT and
  whether a throttled sync request gets no reply. The 27.2 app check (3 clean boots) and 27.1 were not run.
- **First-boot LaunchServices stall:** not reached.

## iOS version per phone (goal 4, done; part of goal 2)
- `cli/libexec/arm64/device-a64.py create --model M --ios 27.2 [name]` writes <device>/runtime; `runtimes` lists
  them; `list` shows each phone's version. Only a 27.0 phone gets a copy of the main phone's data.
- boot-a64.sh boots a device on its runtime, with $SCRATCH/jc-<build> (27.0: jitcache) and placement-<build>.txt;
  A64_RUNTIME still overrides. No scratch script needed (s23/bootrt.sh is gone anyway).
- Launcher: phone list "iPhone 17 · name · iOS 27.2"; the iOS version row is the selected phone's (path in the
  tooltip); Add phone has an iOS picker; Start/Restart pass only the device. MCP sim_add_device takes `ios`;
  sim_devices lists versions and runtimes. Screenshot taken; dark style unchanged.

## Goal 3 (memory, was goal 2)
- **(b) Maps/Health never suspended: fixed.** The simulator build of RunningBoard skips pid_suspend for apps with
  UIBackgroundModes "audio" (-[RBProcess _lock_suspend]: "Ignoring suspend because this process is simulated and has
  background audio capabilities"; confirmed in the Mac simulator's RB log). Maps (audio, location) and Health (audio)
  have it. simshim (runningboardd only) makes -supportsBackgroundAudio answer NO at that one call site (found per
  runtime from its log string); the audio-domain entitlement checks still see the real value. Maps now gets
  suspended after home and jetsam.py reclaims it.
  - Gate g1 (27.0, clean): PSS idle 2280 MB / 46 procs (Speed 23: 2763-2799), with apps 2611 MB / 53 procs (Speed
    23: 3377-3416). Wallet and Photos have no audio mode; they suspend when their background tasks end.
- (a) PosterBoard's 12 poster extensions at boot: not investigated. (c) per-process heap: not started.
- Diagnostics: X86CODE_STATEDUMP (RunningBoard assertions and process states without os_log info level).

## Goal 4 (tap / native ASTC): not started.

## Zero-crash gate
- g1 (00:33, default iPhone 17 / 27.0, OCR-poll times): home 44 s, Settings 31/16 s, About OK, Calendar 6/2,
  Photos 6/2, Maps 13/2, keyboard, Siri, Files ("On My iPhone is Empty"), Health 10 s, Wallet 6 s; 0 CRASH/STALL.
- g2 (00:41) home TIMEOUT. The files were being deleted during it, so g2/g3 are invalid. Not 3 consecutive: the
  zero-crash rule is NOT met this round.

## Data loss (not caused by this round's code, as far as I can find)
Between ~00:41 and ~01:00 on 2026-11-01, /home went from 578 GB to 337 GB used. No /home snapshots exist.
Another interactive session on this machine ("i need to make sure…", started ~23:55) is the likely source; nothing
in boot-a64, the gate scripts, the launcher/aot tool, libarm64jit or fpsim2host (its log shows no unlink calls) does
this. Exact list of what is gone:
- **Repo `runtimes/`:** ios-27.0-24A434-arm64 (contents; an empty RuntimeRoot/System was recreated by a boot at
  01:00), ios-27.1-24A94425-arm64, ios-26.5-23F73-universal (the frozen x86 runtime), dyld-cache-23F73-x86_64,
  dyld-cache-23F73-x86_64-mac. Left: ios-27.2-24B8117-arm64 (27 GB). The repo itself (git, var/, external/,
  darling-prefix/) is intact.
- **~/scratch/x86code:** every directory except site/: jitcache (27.0 AOT image + training), jc-24B8117 and
  jc-24A94425 (27.2/27.1 AOT images), a64prefix, a64prefix-b and their .workdir, a64home, a64testprefix, tessdata,
  s4-s24 (all round scripts, logs, screenshots), round-done/ (speed1-23.md reports), dyldcache, survey*, stage3*,
  videos, applesrc, arm64jit-build and the other build/test dirs; then (after 00:49) the top-level loose files too
  (screenshots, logs, abrt files from before 00:49). Left: site/, abrt-*.txt written at 00:49, and what I created
  after (tessdata/eng.traineddata re-downloaded at 00:56, gate/train270, canary-dir, canary-file.txt).
- Possibly elsewhere under /home (241 GB in total is more than the items above); not checked.
- To rebuild later: `x86sim runtime fetch` for 27.0/27.1 (Mac-free), the 26.5 runtime from the Mac, then a JIT-only
  training boot per runtime (tests/arm64/gate/gate.sh with GATE_SLOW=4 on an empty cache dir) and `x86code-aot build`.

## State left
- No sim running: I stopped the containers I started (default device, 27.2 instance) and my jetsam.py. simview,
  the progress site and simstream were already running and were left alone. Launcher closed (it was closed before).
- The default device is NOT at the home screen: it can't boot until the 27.0 runtime is back.
- No sudo, no host settings changed, tunnel untouched.

STAGE ROUND COMPLETE
