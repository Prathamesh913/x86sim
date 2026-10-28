# Findings

Mac used for lookups: a Mac mini (arm64, macOS 27.0 26A428, Xcode 27.1 27A9275).

## Spike 0: runtime inventory (2026-10-26)

- The runtimes Xcode installs by default on Apple Silicon are the
  "Arm64Only Customer Simulator Runtime" variant (`cryptexVariant` in
  `/Library/Developer/CoreSimulator/Images/images.plist`). iOS 26.5 23F77
  and iOS 27.0 24A434 are both arm64-only.
- The universal variant can be fetched without touching the installed set:
  `xcodebuild -downloadPlatform iOS -buildVersion 26.5 -architectureVariant universal -exportPath ~/simrt`
  This gives iOS 26.5 **23F73**, 10.6 GB, as an exportedBundle containing
  `Restore/iOSSimulatorRuntime_Cryptex.dmg`. Note: it also registered the
  runtime with simctl.
- **iOS 26.5 universal: GO.** All 5,314 Mach-O files in RuntimeRoot are
  `x86_64 arm64` (3 also have `arm64e`). That includes dyld_sim,
  launchd_sim, SpringBoard, backboardd, UIKitCore, QuartzCore and Metal.
- There is no dyld shared cache in the runtime. The host builds it per
  architecture into
  `/Library/Developer/CoreSimulator/Caches/dyld/<host build>/<runtime id>/dyld_sim_shared_cache_<arch>`.
  On Linux we either build an x86_64 cache or run dyld_sim from the
  standalone dylibs.
- The kernel boundary is three host-reexport shims. The runtime's
  libSystem re-exports `libsystem_sim_{kernel,platform,pthread}`, and the
  `*_host.dylib` variants re-export the **host macOS**
  `/usr/lib/system/libsystem_{kernel,platform,pthread}.dylib` (they link
  against libsystem_kernel 10002.1.13). There's also
  `usr/lib/system/host/liblaunch_sim.dylib`. So the simulator brings no
  syscall layer of its own: on Linux, Darling's libsystem_kernel, platform
  and pthread sit exactly at this seam. Risk: Darling's copies are much
  older than the ABI these shims expect.
- **iOS 27.0: no x86-64.** `xcodebuild -downloadPlatform iOS -buildVersion 27.0
  -architectureVariant universal` gives "iOS 27.0 (universal) is not available
  for download." So iOS 26.x is the **last** runtime with x86-64 slices.
  Any app needing an iOS 27 SDK/runtime would require arm64 to x86-64
  translation. Treat iOS 26.5 23F73 as the frozen target, and keep the
  exportedBundle archived locally in case Apple pulls the download.

## Linux-side extraction (no Mac needed)

- Copied the exportedBundle to `runtimes/ios-26.5-23F73-universal/` (9.9 GB).
- Stock `7z` (p7zip/7-Zip with APFS support) reads `iOSSimulatorRuntime_Cryptex.dmg`
  directly: `7z x -snld -oextracted Restore/iOSSimulatorRuntime_Cryptex.dmg`.
  RuntimeRoot is 23 GB unpacked, about 423k entries. `file` on Linux confirms
  `dyld_sim`, `launchd_sim` and `libsystem_sim_kernel_host.dylib` are
  universal x86_64+arm64.
- Note: `-snld` makes absolute symlinks relative to the archive root. They
  were rewritten to relative links inside RuntimeRoot. Five links still
  dangle (four TTS voice files and `IOKit.framework/Modules`); their
  targets aren't in the image, so this doesn't matter.

## Spike 3 prep

- Test binary `tests/probes/hello` (from `hello.c`): built on the Mac with
  `xcrun --sdk iphonesimulator clang -target x86_64-apple-ios26.5-simulator`.
  It's x86_64, LC_BUILD_VERSION platform 7 (iOS simulator), minos 26.5,
  SDK 27.1, and links only `/usr/lib/libSystem.B.dylib`.

## Darling build (Spike 3.0)

Darling at commit 60ba801d (2026-09-06), built with clang 23.1.4, x86_64 only,
RelWithDebInfo, installed to `darling-prefix/` (544 MB). It needed three
local patches/flags for the new compiler:
- objc4 `runtime/CMakeLists.txt`: strip `-DNDEBUG`, because objc4 always
  defines `OBJC_IS_DEBUG_BUILD=1`.
- Global `-fno-objc-constant-literals`: clang 23 emits `NSConstantArray`,
  `NSConstantDictionary` and so on, which Darling's Foundation lacks.
- libunwind `-fno-jump-tables`: clang 23 relative switch tables need chained
  binds, which the bundled ld64 doesn't implement.
Blocker to run: `bin/darling` must be setuid root (it creates mount and PID
namespaces).

## Static gap list: runtime imports vs Darling exports

`tools/debug/host_imports.sh` lists every x86_64 symbol imported somewhere in
RuntimeRoot and defined nowhere in it. That's 890 symbols, of which 814 are
C-level (excluding ObjC/Swift/C++). 725 of those are already exported by
Darling's libsystem_kernel/platform/pthread. The 89 missing
(`analysis/gap_vs_darling.txt`) fall into these groups:

| Group | Count | Notes |
|---|---|---|
| mem*/str* (`memcpy`, `strlen`, `memset_pattern*`, `bzero`, ...) | ~25 | Real macOS exports these from libsystem_platform. Darling has them elsewhere (`_platform_*` / libc). Only aliases are needed. |
| Not host symbols (`RPB*`, `BiomeLibraryInternal`, `mlc_model_compile`, `XCTestMain`) | ~45 | Weak imports from frameworks missing in the sim, or from XCTest. Ignore. |
| **Real kernel/libsystem gaps** | ~20 | `mach_msg2_internal` (the modern mach_msg path, **critical**), `mach_eventlink_*` (4), `os_channel_*` (Skywalk networking, 14), `voucher_mach_msg_clear`, `openat_authenticated_np`, `mkfifoat`, `mknodat`, `posix_spawnattr_setdataless_iopolicy_np`, `pthread_install_workgroup_functions_np`, `_pthread_workqueue_add_cooperativethreads`, `pthread_yield_np`, `work_interval_instance_set_complexity`, `__linkguard_init`, `_platform_memcmp_zero_aligned8` |

Verdict: the **symbol-level** gap is bounded, at tens of items (the Spike 1
"go" criterion). This is a static view only. It says nothing about whether
Darling implements these calls' *behaviour* faithfully (Mach IPC,
`mach_msg2`, workqueues), which is what Spike 3.1 and 3.2 test.

## Spike 3.1: iossimulator binary under dyld_sim on Linux. **PASSED** (2026-10-26)

```
$ cli/libexec/run-sim.sh tests/probes/hello
hello from iossimulator x86_64, pid 37, task port 259
```
This is an unmodified iOS 26.5 x86_64 simulator binary, loaded by Apple's
`dyld_sim` with Apple's libSystem, libdispatch and libobjc initialised,
running on Darling on Linux. It works reliably (3/3 runs) and macOS
binaries under Darling still work.

How to run it: `cli/libexec/run-sim.sh <binary>` sets `DYLD_ROOT_PATH`,
`SIMULATOR_ROOT` and `IPHONE_SIMULATOR_ROOT` to the extracted RuntimeRoot.
Darling's host dyld already contains Apple's `useSimulatorDyld` handoff.

Fixes needed, in the order hit (all in `patches/darling-iossim.patch`):
1. `fcntl(F_ADDFILESIGS_FOR_DYLD_SIM)` gave EINVAL. Accept it and report
   the signed range as reaching the signature blob, as dyld checks.
2. Missing symbols, starting with `mach_eventlink_*` from libdispatch.
   Added `simcompat.c` to three libraries:
   - libsystem_kernel: `mach_msg2_internal` translated to `mach_msg_overwrite`
     (including vector mode as used by libdispatch, aux data reported
     empty), `voucher_mach_msg_clear`, eventlink and work_interval stubs,
     Skywalk `os_channel_*` stubs, `openat_authenticated_np`, `mkfifoat`,
     `mknodat`, `posix_spawnattr_setdataless_iopolicy_np`.
   - libsystem_platform: public `mem*`/`str*` exports and
     `_platform_memcmp_zero_aligned8`.
   - libsystem_pthread: `pthread_yield_np`,
     `pthread_install_workgroup_functions_np`,
     `_pthread_workqueue_add_cooperativethreads`.
3. GPF in `malloc` during `__libkernel_init`. Darling appends
   `dyld_func_lookup`/`posix_spawn_*` to `_libkernel_functions` after v4,
   but Apple's v5+ uses those slots (for `malloc_type_*`). The Darling
   fields are now only used when `version == 4`. A "foreign libSystem"
   instead gets `elf_calls` from mldr's apple strings.
4. `assert_simulator_supported_host` (ud2) requires `kern.osrelease`
   major > 22 (macOS 14+). Report `25.0.0` (macOS 26) to foreign-libSystem
   processes only.
5. `selLock` "recursive" abort in libobjc. libsystem_kernel exported
   **no-op** weak `os_unfair_lock_{lock,unlock}` stubs, which shadowed
   libsystem_platform's real ones in the sim's re-export search order, so
   unlock did nothing. They are now file-local.
6. `access(NULL)` from `os_variant_has_internal_diagnostics` crashed when
   `IPHONE_SIMULATOR_ROOT` was unset. Set the env var; faccessat now
   returns EFAULT for NULL like XNU.

Debugging kit: `tools/debug/lastcrash.sh [pid]` symbolises the newest Darling
core dump, mapping Mach-O frames through the core's NT_FILE note
(`tools/debug/symbolize_core.py`). Note: the first `darling shell` after a
container (re)start must have stdin from /dev/null or it hangs.
Darling's own `iokitd` crash-loops in the background; it's unrelated and
not yet investigated.

## Spike 3.2: launchd_sim bring-up. **PASSED** (2026-10-26)

### How CoreSimulator launches it (read from a booted sim on the Mac)
`research/macref/` holds the raw data. `launchd_sim` gets one argument, the
path to `<device>/data/var/run/launchd_bootstrap.plist`, and about 35
`SIMULATOR_*` env vars. Its parent is the host's `/sbin/launchd`. Plist keys:
`Bootstrap.{Self,Child,ChildDefaults}` (env), `Bootstrap.Paths` (job plist
dirs), `SimulatorRuntimeRoot`, `AbortOnUnknownIPC`, `RequireDomainBinding`,
and so on. The host bootstrap name is `XPC_SIMULATOR_LAUNCHD_NAME =
com.apple.CoreSimulator.SimDevice.<UDID>`. A stock iPhone 16 boot ran 203
child processes and 382 launchd services.
`cli/libexec/mksimdevice.py <dir> <labels...>` generates an equivalent device
with only the named jobs. `cli/libexec/run-launchd-sim.sh <dir>` starts it under
Darling.

### Done so far (all in patches/darling-iossim.patch)
- **kqueue in sim processes.** Darling implements kqueue in user space
  (libkqueue) inside *its* libSystem, which overrides weak hooks in
  libsystem_kernel. Fix: build `libkqueue_sim.dylib` (exports
  `__darling_sim_*`) and inject it with `DYLD_INSERT_LIBRARIES`.
  libsystem_kernel resolves the hooks through `_libkernel_functions->dlsym`
  when the libSystem is foreign (`foreign_hook.h`). dyld_sim only loads
  iOS-simulator-platform images, so the dylib is retagged
  (`tools/debug/set_macho_platform.py`, run by `tools/debug/dinstall.sh`).
  `tests/probes` passes.
- **Kevent workqueue (WORKQ_FEATURE_KEVENT).** iOS 26 libdispatch requires
  it, and Darling's version was synchronous and disabled. Added an
  XNU-style asynchronous one for foreign-libSystem processes only:
  registrations return immediately (EV_RECEIPT for errors); a manager
  thread waits on the workq kqueue and hands batches to workqueue threads.
  A knote qos side-table routes `_PTHREAD_PRIORITY_EVENT_MANAGER_FLAG`
  knotes to a `WQ_FLAG_THREAD_EVENT_MANAGER` thread (needed for timers).
  In-flight batches are matched by events pointer, because Darling's
  single `wq_event_pending` got clobbered. Feature bits
  (KEVENT|MAINTENANCE|FINEPRIO|QOS_DEFAULT|BSDTHREADCTL|SETSELF|DISPATCHFUNC)
  are reported only to foreign libSystems. `tests/probes` (global async,
  dispatch_after, read source) passes 3/3.
- Fixed a regression from 3.1: libsystem_kernel's private
  os_unfair_lock uses (workq parked list, etc.) now use libsimple's real
  lock instead of no-ops.

### Current blocker: kernel workloops
`launchd_sim` (Swift) creates a `DispatchWorkloop`
(libswiftDispatch → `dispatch_workloop_create_inactive`). iOS 26
libdispatch services workloops through the kernel: `kevent_id` (syscall
375) with `KEVENT_FLAG_WORKLOOP`, per-workloop dynamic kqueues, and
`EVFILT_WORKLOOP` knotes (thread requests, `NOTE_WL_SYNC_WAIT/WAKE` for
`dispatch_sync`, ownership/QoS overrides). Workloop threads
(`WQ_FLAG_THREAD_WORKLOOP`) are delivered to `__libdispatch_workloopfunction`.
Darling has none of this (syscall 375 returns ENOSYS). libdispatch
crashes: "Unexpected error from kevent" (errno 78) in `_dispatch_kq_poll`
on the manager queue.

Follow-up: a shim (`host/simshim/diag/workloop_shim.c`) interposing
`dispatch_workloop_create*` to return serial queues is loaded but **does
not help**. Tracing `kevent_id` (logged to the container's
`/private/tmp/x86code-kevent-id.log`) shows the id is an ordinary dispatch
queue object:

    kevent_id: id=0x7FCAEF708380 nchanges=1 nevents=1 flags=0x403
      change ident=0x7FCAEF708380 filter=-17 (EVFILT_WORKLOOP) flags=0x5 fflags=0x111 qos=0x800008FF

iOS 26 libdispatch uses a kernel workloop as the "wlh" for *every serial
queue hierarchy* (including XPC connections and Mach channels), even
though we don't advertise `WORKQ_FEATURE_WORKLOOP`. **Kernel workloop
emulation is therefore required for any XPC daemon, launchd_sim included.**
This is the "Mach IPC/dispatch fidelity" hardest-bit from the plan, and
it's now concrete. It needs:
- `kevent_id` with per-id dynamic kqueues
  (`KEVENT_FLAG_DYNAMIC_KQ_MUST_[NOT_]EXIST`).
- `EVFILT_WORKLOOP` knotes: `NOTE_WL_THREAD_REQUEST` (ask for a servicer
  thread at a QoS), `NOTE_WL_SYNC_WAIT`/`NOTE_WL_SYNC_WAKE` (blocking
  `dispatch_sync`), `NOTE_WL_END_OWNERSHIP`, `NOTE_WL_DISCOVER_OWNER`,
  `NOTE_WL_UPDATE_QOS`, and the ESTALE protocol.
- Workloop threads: `WQ_FLAG_THREAD_WORKLOOP` with the kqid delivered to
  `__libdispatch_workloopfunction`; parking/return via `kevent_id` with
  `KEVENT_FLAG_PARKING` (Darling's macOS 11 libpthread already has the
  user-space side).
- Other knotes (Mach port receive, timers, fds) registered on a workloop
  must be delivered on that workloop's servicer thread.

### Result

launchd_sim boots under Darling, loads the device's jobs, spawns them through
xpcproxy_sim and launches daemons on demand. Test: a RunAtLoad client
(`tests/probes/prefs`, iossimulator x86_64) writes a preference via
CFPreferences; launchd_sim starts cfprefsd "because ipc (mach)", cfprefsd writes
`data/Library/Preferences/com.x86code.prefstest.plist` and
`CFPreferencesAppSynchronize` returns true. The same binary (arm64 build) gives
the same result on the Mac's simulator.

    cli/libexec/mksimdevice.py var/device com.apple.cfprefsd.xpc.daemon com.x86code.prefs=tests/probes/prefs
    TIMEOUT=50 cli/libexec/run-launchd-sim.sh var/device
    # launchd_sim log: <container>/private/var/tmp/com.apple.CoreSimulator.SimDevice.<UDID>/launchd.log
    # job output: var/device/data/var/log/<label>.log

### What it took

Kernel side (Darling libsystem_kernel / darlingserver / libkqueue):

- **Workloops** (`kevent_id`, syscall 375) emulated in `kqueue/kevent_qos.c`:
  thread requests with debounce, knotes owned by a workloop (registered on the
  workq kqueue, routed by the manager thread), one servicer per workloop
  (`WQ_FLAG_THREAD_WORKLOOP`, id stored before the event batch), sync
  wait/wake knotes (live until EV_DELETE; a wake with EV_ADD may create one
  pre-woken). No priority inheritance/ownership. `WORKQ_FEATURE_WORKLOOP`
  advertised. Trace: `#define WLTRACE` -> per-process
  `/private/tmp/x86code-workloop.<pid>.log`.
- `kern.bootargs` sysctl (empty); string sysctls now count the NUL (XNU does).
- Audit token `val[7]` (pid version) non-zero per task (libxpc checks it).
- `proc_pidinfo` returns structure sizes like XNU (was 1); flavor 18
  (`PROC_PIDT_BSDINFOWITHUNIQID`) added. libdispatch needs flavor 17 = 0x38.
- Mach trap 70 `host_create_mach_voucher` routed to darlingserver; bank-only
  recipes (no bank manager in darlingserver) yield the null voucher.
- `posix_spawn` port actions: special ports (via new darlingserver call
  `spawn_set_special_port`, which accepts immovable rights like XNU's
  `exec_handle_port_actions`) and registered ports (SETEXEC). This is how
  xpcproxy_sim gives a job its bootstrap port.
- libkqueue EVFILT_MACHPORT receive buffer 64 B -> 16 KB per Mach-port knote
  (everything came back MACH_RCV_TOO_LARGE).
- darlingserver: skip `filt_machport_turnstile_prepare_lazily` (asserted when
  a received message carried a receive right).
- `__mac_syscall` (381): Sandbox `sandbox_check` (2) and
  `sandbox_passthrough_access` (12) answer "allowed"; container path (4) fails
  with ENOTSUP as on a Mac; anything else logged once, ENOSYS. cfprefsd refused
  writes ("No write access to parent directory") until call 12 answered.

Host side (stands in for CoreSimulator / host launchd):

- `host/simhost/simhost` (macOS x86_64, built on the Mac with
  `build/mac-build-simhost.sh`): owns launchd_sim's bootstrap port, answers its
  MIG 708 request with a fresh receive right (the domain's service port; simhost
  keeps a send right), forks the "holding tank" child launchd_sim reaps
  (`XPC_SIMULATOR_HOLDING_TANK_HACK`). `run-launchd-sim.sh` uses it.
- Container needs `/private/var/tmp` (dinstall.sh creates it).
- Test jobs: xpcproxy_sim prefixes program paths with the runtime root, so
  mksimdevice.py climbs out with exactly the root's depth of `..` (Darling
  resolves `..` lexically; overshooting escapes the container root).

Debug kit added: `tools/debug/allstacks.sh <pid>` (per-thread stack scan of a core;
the faulting thread is the one in `sigexc_handler`), `tools/debug/xpcproxy_error.sh`
(xpcproxy_sim exits 78 after formatting its reason; strace makes its exit fail
so the core holds the message), launchd_sim's fatal message is the string at
`*(char**)0x10005eed0` in its core, mksimdevice.py `X86CODE_KEEPALIVE=1`
(respawn test jobs), `X86CODE_JOBLOG=1` (runtime jobs log + os_log to
`data/var/log/<label>.log`), `X86CODE_INSERT=<dylibs>`.

### Known loose ends

- `persona` syscall (494) unimplemented; `proc_pidinfo` flavor 24/25 (exit
  reason) unimplemented (launchd_sim asserts, harmless).
- The system domain never leaves bootstrap mode (on the Mac it does after the
  first spawns); jobs here are all in user/<uid>, so not yet a problem.
- logd / notifyd / diagnosticd / FSEvents not in the device yet (lookups fail).
- `timeout` does not stop a running `darling shell`; kill simhost to stop a run.
  Killing it can leave stale launchd_sim/job processes from the old container;
  kill them before the next run.
- darlingserver aborted twice during shutdowns after killing runs (not
  investigated).

### Next (Spike 3.3)

Grow the device toward simslim's service set (logd, notifyd, configd_sim,
...), fix what breaks, then a UIKit app.

## Spike 3.3: growing the device (in progress, 2026-10-26)

Device used: cfprefsd, notifyd, configd_sim, runningboardd, lsd, containermanagerd
(+ .system), mobilegestalt, UserEventAgent-System, securityd, trustd, syslogd and the
prefs test job:

    X86CODE_JOBLOG=1 cli/libexec/mksimdevice.py var/device com.apple.cfprefsd.xpc.daemon com.apple.notifyd \
        com.apple.configd_sim com.apple.runningboardd com.apple.lsd com.apple.containermanagerd \
        com.apple.containermanagerd.system com.apple.mobilegestalt.xpc com.apple.UserEventAgent-System \
        com.apple.securityd com.apple.trustd com.apple.syslogd com.x86code.prefs=tests/probes/prefs
    TIMEOUT=60 cli/libexec/run-launchd-sim.sh var/device

State: see the end of this section.

### Fixed on the way

- **Process tooling.** `timeout` does not stop a running `darling shell`, and killing
  processes inside a running container leaves darlingserver in a state where new jobs
  hang. `run-launchd-sim.sh` now starts from a fresh container (`darling shutdown`).
  Read launchd_sim's log from the host (`~/.darling/private/var/tmp/...SimDevice.../launchd.log`),
  not through another `darling shell` (that can hang). `tools/debug/allstacks.sh` accepts
  `gcore` files; `symbolize_core.py` maps overlay paths and skips unreadable files.
- **System domain** never leaves bootstrap mode here (on a Mac something, presumably
  CoreSimulator, ends it). mksimdevice.py drops `LimitLoadToSessionType: System` so those
  jobs load into the user domain.
- **logd / diagnosticd left out**: with logd present every process crashes in
  `firehose_buffer_create` (logd hands out a shared-memory buffer; darlingserver's
  `mach_make_memory_entry` & co. are stubs). Needs memfd-backed VM in darlingserver.
- **S2C deadlock (runningboardd hung the whole device).** Receiving a *complex* Mach
  message (out-of-line memory, port rights) through a kqueue made darlingserver map
  memory in the receiver from a kqchan microthread via an S2C signal to another thread
  of the process; that never completed. darlingserver now reports such messages as
  `MACH_RCV_TOO_LARGE` (left queued) and libdispatch receives them with `mach_msg`
  (`ipc_pset.c`, `__DARLING__`). Also: libkqueue identifies threads with
  `sys_thread_selfid` (in simulator processes `__thread_selfid` binds to the runtime's
  libSystem), and new threads unblock the SIGEXC/S2C signals (`sigexc_thread_setup`).
- **Mach trap 43** `mach_generate_activity_id` (libdispatch voucher activities).
- **getattrlist** rewritten: common/dir/file/extended-common attributes from stat, name
  and full path, `ATTR_CMN_RETURNED_ATTRS`, `FSOPT_PACK_INVAL_ATTRS`, volume attributes
  from statfs incl. capabilities (was 4 attributes, EINVAL otherwise; Apple's realpath,
  NSURL and containermanagerd use it).
- **renameatx_np** (488) via renameat2 (`RENAME_SWAP`/`RENAME_EXCL`).
- **F_GETPROTECTIONCLASS / F_SETPROTECTIONCLASS**: class D (no protection).
- **csops** reads the target's code signature: identity, team id, entitlements (XML/DER),
  blob; status flags like a simulator process on a Mac; per-binary cdhash (FNV of the
  CodeDirectory, not SHA). Simulator binaries carry their real entitlements in
  `__TEXT,__entitlements` / `__ents_der` (signature plist is empty); preferred when
  present. Note the runtime's `libsystem_sim_kernel` implements csops itself for
  entitlements (reads the section via `proc_pidpath`) and only falls back to the kernel.
- `__mac_syscall` Sandbox calls answered (check/passthrough allowed, container path
  ENOTSUP), `posix_spawn` port action 3 (watch ports) ignored quietly.

### lsd vs containermanagerd (resolved)

containermanagerd refused lsd (NOT_ENTITLED_FOR_OPERATION, 55) while csops' status path
could leave its section descriptor at 0 (closing fd 0, wrong flags). With that fixed,
lsd starts, stays up and containermanagerd answers it. Note the runtime's
`libsystem_sim_kernel` reads simulator entitlements from the binary itself and only
falls back to the kernel's csops.

State after the last run: prefs `synchronize: 1`; cfprefsd, notifyd, syslogd,
runningboardd, containermanagerd_system and lsd stay up. Still crashing: configd_sim
(SIGABRT, `store_host != NULL`: wants the host's configd) and UserEventAgent (SIGILL).

Regenerate `patches/darling-iossim.patch` (Darling + its submodules, incl. darlingserver). Since 2026-10-28 12:24
the changes are committed on `x86code` branches (author designgears), so each repo is diffed from its last upstream
commit (the newest one not by designgears) to the working tree, committed and uncommitted changes together:

    cd darling && base() { git log --format="%h %an" | awk '$2!="designgears"{print $1; exit}'; } && (git diff --ignore-submodules=all $(base); for m in src/external/xnu src/external/libplatform src/external/libpthread src/external/objc4 src/external/libunwind src/external/libkqueue src/external/darlingserver src/external/iokitd src/external/dyld; do (cd $m && git add -N . 2>/dev/null; git diff --src-prefix=a/$m/ --dst-prefix=b/$m/ $(base)); done) > ../patches/darling-iossim.patch

(iokitd and dyld were missing from the list before Spike 3.13.)

## Spike 3.4 — first pixels (2026-10-27)

**Milestone:** backboardd's CoreAnimation (software renderer) draws the boot logo into our
own SimFramebuffer; frames are dumped to `data/framebuffer.bgra` and converted with
`cli/libexec/fb2png.py` (see `docs/screens/`).

Display path:
- `host/simfb/simfb.c` is our x86_64 `SimFramebuffer.framework` (one display, 1179x2556,
  60 Hz, 'BGRA'), loaded via `SIMULATOR_FRAMEBUFFER_FRAMEWORK`. With the variable unset,
  SimFramebufferClient dlopen(NULL)s and `SFBConnectionCreate` calls itself forever.
- The display **must be named "LCD"**: CA clients pick `[CADisplay mainDisplay]` by that name
  (otherwise SpringBoard asserts on a nil display).
- `IOSURFACE_REMOTE_EMULATED=1` makes IOSurface allocate in-process (no host service);
  cross-process surfaces still need `com.apple.IOSurface.Remote` (docs/protocols/IOSURFACE.md).

Darling / darlingserver fixes:
- Mach memory entries + `mach_vm_map` of them, memfd-backed (duct-tape memory.c).
- `thread_unblock` now cancels the wait timer (XNU does): a stale timer aborted later untimed
  waits with THREAD_TIMED_OUT (MACH_RCV_TIMED_OUT crashes, ipc_mqueue_send assert).
- kqchan Mach-port channel: a notification queued before a modify reply was read as the reply
  and lost, leaving the port silent forever ("server idle, client waiting": cfprefsd, notifyd,
  launchd_sim). Modify now acknowledges notifications; client skips them and reads
  notifications non-blocking.
- `__ulock_wait` returns real remaining-waiter counts (pthread_dependency_wait_np crashed on 1).
- `NOTE_WL_SYNC_IPC` knotes accepted as no-ops; `kqueue_workloop_ctl` (530) succeeds;
  `memorystatus_control` (440) succeeds; workqueue threads no longer exit after 15 s idle.
- Host dyld reports AMFI flag 0x40 (library interposing): newer dyld_sim ignored every
  `__interpose` section without it — DYLD_INSERT shims silently did nothing before.

Runtime fixes / workarounds (host/simshim/compat/simshim.c):
- `csops`/`csops_audittoken` entitlement blobs go to Darling's syscall: libsystem_sim_kernel's
  versions fail a short buffer with ENOENT instead of ERANGE, so large entitlement plists
  (lsd, SpringBoard) read as empty (containermanagerd "not entitled", RBS assertion refusals).
- WORKAROUND `xpc_init_services` skipped (launchd_sim never answers the pid-domain uncork).
- `os_state_add_handler` dummy handles when os_log is disabled.
- 80 symlinks lost when the runtime was extracted (empty files, e.g. SafariServices.framework)
  restored from the Mac; list in runtimes/ios-26.5-23F73-universal/restored-symlinks.tsv.

Logging: `OS_ACTIVITY_DT_MODE` streams every log line to diagnosticd; with diagnosticd absent
or stalled, senders loop/block. `X86CODE_NODT=all` (mksimdevice) disables DT mode and sets
`OS_ACTIVITY_MODE=disable`.

Open: SpringBoard waits on lsd, which waits on the user containermanagerd ("database is locked"
on references.sqlite3, then trap); the launchd_sim uncork; backboardd's stale-unote crash when a
client dies; diagnosticd crash in libkqueue knote_lookup; UserEventAgent SIGILL.

Tools: tools/debug/sigctx.py (fault registers from Darling's alternate signal stack), tools/debug/stuck.sh,
tools/debug/gcoresim.sh, cli/libexec/fb2png.py, host/simshim/diag/syncdiag.c (logs sync XPC calls).

### lsd registration -43 (fixed 2026-10-27)

lsd failed every registration with -43 (fnfErr), so the csstore held only extension points and
SpringBoard aborted in `-[SBApplicationController _loadApplications:remove:]`. Cause (probe
tests/probes/fsprobe.c, compared with the Mac): `-[FSNode getVolumeIdentifier:error:]` failed
(-10813) because CFURL's NSURLVolumeURLKey failed. Darling's statfs/fstatfs/getfsstat reported
Linux mount points ("/home", not "/Volumes/SystemRoot/home"), fstatfs matched the mount table
against the prefix-view path, and f_fsid was Linux's (CF matches a file's fsid {st_dev,0}
against getfsstat). Fixed in libsystem_kernel emulation stat/statfs.c, fstatfs.c, getfsstat.c
(mount points in the prefix view, octal unescaping, whole-component match, f_fsid = {st_dev,0}).
Also getxattr returns the attribute length (was 0) and honours XATTR_NOFOLLOW.
Now: lsd writes a ~6 MB csstore with com.apple.springboard; SpringBoard gets past
_loadApplications and dies with SIGILL "BUG IN LIBDISPATCH: Invalid wlh state"
(_dispatch_queue_drain_try_lock_wlh on a workloop servicer thread: the workloop is drain-locked
by another thread and not ENQUEUED, i.e. the workloop emulation in kqueue/kevent_qos.c delivered
a stale or duplicate thread request). Still open: Darwin xattr names (com.apple.*) get
EOPNOTSUPP from Linux (no "user." namespace mapping); fsgetpath/openbyid_np unimplemented.

### Workloop "Invalid wlh state" (fixed 2026-10-27)

SpringBoard died with SIGILL "BUG IN LIBDISPATCH: Invalid wlh state": a workloop servicer was
started for a workloop that was drain-locked by another thread and not ENQUEUED. Our workloop
emulation (kqueue/kevent_qos.c) differed from XNU's filt_wl* in three ways:
- The NOTE_WL_THREAD_REQUEST knote is EV_CLEAR in XNU (filt_wlattach): delivering it consumes
  it until the next touch. We redelivered it on every bind (after every servicer return or
  knote event) while it existed, i.e. stale thread requests.
- Ownership was ignored: XNU records `kqwl_owner` from NOTE_WL_DISCOVER_OWNER (the drain-lock
  port name in dq_state, uncontended dispatch_sync) and starts no servicer until that thread
  ends ownership (NOTE_WL_END_OWNERSHIP). We started one at once, racing the owner.
- Changes returned by a servicer (WQOPS_THREAD_WORKLOOP_RETURN, i.e. KEVENT_FLAG_PARKING) were
  applied with no event list: the first error aborted the rest and errors were dropped. XNU
  hands them back to the servicer (libdispatch redrives an ESTALE "leave"). Now they are
  queued and delivered to the next servicer. Error events also keep their flags and carry the
  loaded dq_state in ext[VALUE] (libdispatch redrives from it), as in filt_wlupdate.
Port vs fix: willi/darling-xnu#2 (+#6, #11) reimplements the kevent workqueue (~12k lines,
moves libkqueue into libsystem_kernel, needs their darlingserver/Libsystem/libkqueue forks)
and would replace our fixes (reusable wl_table, sem rewrite, inflight batches, sigexc on the
manager). Their design confirmed the missing semantics (thread_request_active, owner), so we
added those to ours instead (small, same XNU behaviour for what libdispatch uses).
Result: SpringBoard no longer crashes (two boots, 240 s, one spawn, no "exited due" for it);
cfprefsd/lsd/runningboardd/backboardd up. Frame still 4, boot logo only (black/white/gray).
Next: SpringBoard's main thread blocks in a sync NSXPC call to com.apple.systemstatus.publisher
(systemstatusd not in the boot list; the lookup fails but the sync send never errors out, unlike
other missing services). With `com.apple.systemstatusd` added, SpringBoard gets further and
crash-loops (21x in 240 s) with SIGILL in libsqlite3 fts3TokenizerFunc
(ITSRegisterSQLiteICUTokenizer <- -[ATXInformationStore _openDatabase], AppPredictionClient).
Also SpringBoard logs thousands of "Unimplemented mach trap (11)" (mach_vm_purgable_control).

### systemstatus deadlock, fts3 SIGILL, SQLite "database is locked" (fixed 2026-10-27)

- **Sync XPC to com.apple.systemstatus.publisher never returns** (systemstatusd not running).
  Not an emulation bug: SystemStatus makes the sync NSXPC call *on the connection's own queue*
  (`com.apple.systemstatus.publisher.internalqueue`, logged by syncdiag). libxpc delivers the
  lookup failure / invalidation on that queue, so the call deadlocks — native macOS does
  the same (tests/probes/xmissprobe2.m cases 6-8 hang on the Mac too; cases 0-5 and
  xmissprobe.c, incl. OOL/port/endpoint payloads, error at once on both). launchd_sim
  evaluates `#IfFeatureFlagEnabled SystemStatus/EmbeddedDaemon` correctly (the flag is
  FeatureComplete, so systemstatusd owns the name). Fix: com.apple.systemstatusd added to
  cli/libexec/rundev.sh's default jobs.
- **SIGILL in fts3TokenizerFunc**: Apple's fts3_tokenizer() checks each tokenizer method
  pointer with `mincore(ptr, 8)` and runs `ud2` if it fails. Linux mincore fails unaligned
  addresses with EINVAL; XNU truncates to the page. Fixed in emulation mman.c (sys_mincore).
- **SpringBoard `_exit(3)` loop** (`-[_PASSqliteDefaultErrorHandler _handle_SQLITE_LOCKED...]`,
  ATXInformationStore BEGIN EXCLUSIVE; also ~1000 "database is locked" on
  FrontBoard/applicationState.db): with a busy timeout set, Apple's SQLite (unixFileLock) locks
  with `F_OFD_SETLKWTIMEOUT` (93), falling back to `F_SETLKWTIMEOUT` (10). Darling returned
  EINVAL without a syscall (invisible in strace), which SQLite reports as SQLITE_BUSY. Now
  emulated in fcntl.c (retry F_OFD_SETLK/F_SETLK with backoff until the timeout, then ETIMEDOUT,
  as XNU's lf_setlock). Verified: tests/probes/sqlprobe3.c (busy_timeout + a timed-out
  conflict) matches the Mac.
- `mach_vm_purgable_control` trap (11) implemented (everything nonvolatile, never purged);
  SQLite's purgeable page cache called it constantly ("Unimplemented mach trap (11)").
- syncdiag.c now also logs the connection + current queue, failing sqlite3_step (SQL, codes),
  sqlite3_open_v2, SQLite's error log, and exit/_exit backtraces.

Result (240 s boot): SpringBoard spawned once and stays up, launches Spotlight
(UIKitApplication:com.apple.Spotlight, SIGILL). Frame 9 (docs/screens/11-latest.png): black
with a gray activity spinner in the middle, no colour. New: backboardd SIGILL 5x in 240 s.
Next: backboardd and Spotlight SIGILLs (tools/debug/sigctx.py on the cores), "Failed to confine fd"
(SQLite's F_SETCONFINED, EINVAL, only a warning), Unimplemented syscall 165 (quotactl).

### Stale dispatch unotes: defer-delete; home screen (2026-10-27)

- **backboardd SIGILL (15x/240 s) "API MISUSE: Resurrection of an object"** in
  `_dispatch_kevent_merge` (libdispatch+0x26b01), plus backboardd SIGSEGVs and UserEventAgent
  SIGILL/SIGSEGV: an EVFILT_MACHPORT event (flags 0x390, EV_DISPATCH2|EV_ONESHOT, a mach reply
  unote) merged into a freed unote. XNU's kevent_register defers deletes: EV_DELETE without
  EV_ENABLE on a *disabled* EV_DISPATCH2 knote (its event was delivered and may still be in a
  servicer's list) fails with EINPROGRESS; libdispatch keeps the unote until it merges that
  event, then deletes with EV_DELETE|EV_ENABLE. libkqueue deleted at once, so libdispatch freed
  the unote while our kevent manager still held the event. Fixed in libkqueue
  src/common/kevent.c (kevent_copyin_one). This is the "stale unote when a client dies" crash.
- Also (XNU behaviour, not proven necessary for this crash): only one event-manager workqueue
  thread at a time. `_dispatch_mgr_queue_poke` requests and manager kevent batches each got
  their own thread; now further requests queue until the running manager returns
  (workq_kernreturn.c wq_mgr_claim/wq_mgr_release; the thread's WQ flags are kept in TSD slot
  `__PTK_DARLING_WQ_FLAGS`, set in bsdthread_register.c).
Result (240 s): backboardd, SpringBoard, UserEventAgent spawned once, no crashes; **SpringBoard
draws the home screen** (docs/screens/12-home-screen.png: status bar "Carrier"/time/battery,
two "Waiting..." widgets, Calendar/Photos/Reminders/News/Maps/Health/Wallet/Settings with blank
gray icons, page dots, 2 dock icons).
Next: Spotlight (launched by SpringBoard) SIGILL "BUG IN CLIENT OF LIBDISPATCH: Unexpected
EV_VANISHED (do not destroy random mach ports)" in `_dispatch_mach_recv_direct_merge_evt`:
darlingserver's kqchan reports a vanished receive right (duct-tape kqchan.c knote_vanish) for a
dispatch_mach channel's recv port — find who destroys that port. Icons/widgets are empty
(icon rendering / widget extensions not running).

### Spotlight EV_VANISHED, icons, widget launch (2026-10-27)

- **Spotlight SIGILL "Unexpected EV_VANISHED" and backboardd "Resurrection of an object"** were one
  bug: both events had flags 0x390 (EV_VANISHED|EV_DISPATCH2|EV_ONESHOT), i.e. darlingserver's fake
  vanish event, merged into a unote libdispatch had already deleted. Diagnosis: knote_vanish logs the
  owner pid/port/backtrace (debug level now), host/simshim/diag/portdiag.c (X86CODE_INSERT) logs every
  receive-right destruction with a backtrace. Sequence: libxpc cancels a connection (EV_DELETE
  succeeds), its disconnect handler destroys the port (`_xpc_connection_handle_disconnect_event` ->
  mach_port_destruct, a legitimate destroy by the owner, not willi#3's teardown and not a renamed
  port), darlingserver marks the still-open kqchan vanished, and the workq manager thread in
  kevent_qos.c had already copied that event out of libkqueue (or does so before the close is
  seen) and then queued it for a servicer although the knote was gone. XNU drops a deleted knote's
  activation. Fix (kqueue/kevent_qos.c): deletes are recorded only once they succeed (a deferred
  EINPROGRESS delete keeps its in-flight event), and events of knotes no longer registered are
  dropped at copyout, in wl_post_event (under wl_lock, ordered against the purge) and in wq_deliver.
  Result: no crashes in 240 s boots (backboardd, Spotlight, UserEventAgent spawned once).
- **iconservicesagent SIGSEGV** (xpc_retain(NULL) in -[ISGenerationResponse encodeWithCoder:]):
  xpc_shmem_create_readonly got NULL because darlingserver refused the memory entry ("memory entry
  over partially shared region not supported"). Shared-entry intersection treated touching ranges as
  overlapping, and entries are never dropped on munmap, so reused addresses overlapped stale ones.
  Fixed in duct-tape memory.c (half-open ranges; overlapped entries are forgotten and the region is
  shared afresh). IconRendering has no Metal ("No Metal renderer available"; the sim's Metal is the
  host's com.apple.metal.simulator), so it renders in software and cannot write its icon cache
  ("TextureIOError.usingSoftwareRenderer"), but the images still reach SpringBoard by xpc_shmem.
  No IOSurface sharing is needed for icons.
- **Daemons**: cli/libexec/rundev.sh now also starts iconservicesagent, chronod, sleepd, pkd, distnoted,
  tccd, fontservicesd (all run on a real iOS 26.5 sim, `launchctl list`). rundev.sh keeps the
  spawn/exit lines in /tmp/rundev-spawns.log because launchd.log rotates when failed lookups flood it.
- **WidgetRenderer launch refused** by runningboardd ("originator doesn't have entitlement
  com.apple.runningboard.primitiveattribute"): RunningBoard strips restricted entitlements unless
  `(csstatus & (PLATFORM_PATH|PLATFORM_BINARY|VALID)) == PLATFORM_BINARY|VALID`. Darling reported
  CS_PLATFORM_PATH with PLATFORM_BINARY, and sim runtime binaries (ad-hoc signed without the
  CodeDirectory platform field, or unsigned) not as platform binaries at all. A real sim's
  SpringBoard: valid, kill, allowed mach-o, platform dyld, entitlements validated, platform binary.
  csops.c now reports that for anything under ".../RuntimeRoot/" (probe tests/probes/csprobe.c).
  WidgetRenderer_Default now launches.
- **Colour**: the framebuffer path is fine ('BGRA' is right): icons render in colour
  (docs/screens/13-color-icons.png). The wallpaper is black because nothing draws one yet
  (PosterBoard is an app launched on demand; not investigated).
- Note: after deleting the csstore, the first boot shows gray icons (lsd rebuilds); the next shows them.

Screens: 13-color-icons.png (home screen with coloured icons for Photos, Reminders, News, Maps,
Health, Wallet, Settings, Safari; Calendar (dynamic icon) and the second dock icon still gray;
widgets still "Waiting...").

Next: widgets. chronod: "Extension not found for com.apple.news::com.apple.news.widget" etc.;
`LSApplicationExtensionRecord initWithBundleIdentifier:` fails with -10814 in
_LSPluginFindWithPlatformInfo, while the same records are reachable from their LSApplicationRecord
(tests/probes/exprobe.m). Every record (apps too) has platform 0 on Darling, 7 on the Mac.
LaunchServices sets LSExecutablePlatformKey in _LSParseLoadCommands from dyld_get_image_versions on
a copy of the executable's header; that call works in a probe (tests/probes/dvprobe.c), and a fresh
csstore still has platform 0, so find why lsd's parse path does not run or set it (lsd with
OS_ACTIVITY_DT_MODE, or break on _LSParseLoadCommands). Also: launchd.log is flooded with failed
lookups (com.apple.diagnosticd from Spotlight, com.apple.metal.simulator from backboardd,
locationd/profiled/... from SpringBoard) and launchd_sim's log stopped ~1-2 min into one boot.

### LaunchServices platform 0 / extensions -10814: file IDs (fixed 2026-10-27)

Cause: lsd never parsed executables. `_LSCreateRegistrationDataForDirectoryNode` creates an FSNode for the
bundle executable with flags 1 (`checkResourceIsReachableAndReturnError:` -> `getFileIdentifier:` ->
CFURL `_kCFURLFileIDKey`); that key was NULL on Darling, so FSNode init failed (-10813),
`_LSAddExecutableFormatInfo` never ran, there was no LSExecutablePlatformKey in the info/plugin dicts,
every record (apps and plugins) got platform 0, and `_LSPluginFindWithPlatformInfo` (which matches the
plugin's platform against the caller's, 7) found no extension by bundle ID. (dyld_get_image_versions
and `_LSParseLoadCommands` itself were fine: tests/probes/lsparse.c calls the private function directly.)
CoreServicesInternal's parseAttributeBuffer only keeps a file ID if the volume (MountInfo capabilities)
has VOL_CAP_FMT_PATH_FROM_ID. Fixes (libsystem_kernel emulation):
- getattrlist_generic.c: volumes report PERSISTENTOBJECTIDS and PATH_FROM_ID; ATTR_CMN_DATA_PROTECT_FLAGS
  and CMNEXT_CLONEID are returned (CF requests them; the Mac returns everything asked);
  ATTR_CMN_PARENTID was 0 for every non-directory ("..": ENOTDIR), now from the fd's Linux path.
- misc/fsgetpath.c: fsgetpath (427) was ENOTSUP; it now resolves (fsid, ID) pairs that getattrlist
  handed out in this process (256-entry cache; Linux has no inode -> path lookup).
- conversion/stat/common.c: stat64 st_birthtimespec was uninitialized (garbage CRTIME/ADDEDTIME);
  now the earlier of mtime/ctime.
Diagnosis tools: host/simshim/diag/lsdiag.c (lsd only: dyld_get_image_versions callers, LSExecutablePlatformKey
lookups, opens, FSNode swizzles), host/simshim/diag/attrdiag.c (getattrlist requests/returned bitmaps/buffers,
optional format-cap override from /tmp/fmtcaps), tests/probes/lsparse.c. `X86CODE_NODT=<all labels but
com.apple.lsd>` gives lsd's os_log in its log without hangs.

Result (DB rebuilt, two 240 s boots): exprobe: News/Maps records platform 7, sdk 26.5;
`LSApplicationExtensionRecord initWithBundleIdentifier:` finds com.apple.news.widget etc. chronod now
launches ~30 widget extensions (Maps, Calendar, Reminders, Health, ... via pid/<chronod> jobs) and
WidgetRenderer_Default. The Messages dock icon (MobileSMS was missing for the same reason) is now
coloured. Screens: 14-platform7.png (first boot after DB rebuild: widget titles "Maps"/"Calendar"
instead of "Waiting...", icons gray while lsd rebuilds), 15-widgets.png (second boot: coloured icons,
Messages in the dock, two white widget platters with no content).

Still open / next: widget platters stay white (WidgetRenderer_Default SIGSEGV/SIGKILL, PassbookWidgets
and ScreenTime extensions SIGKILL; check chronod/WidgetRenderer logs and the cores); SpringBoard
SIGSEGV once in the second boot; Calendar icon still gray (dynamic icon, not investigated);
wallpaper/PosterBoard not investigated; "could not prewarm data backed property list: 22" x20k in lsd;
fsctl (242) unimplemented.

### Widgets, fileports, xattrs, PosterBoard (2026-10-27)

- **SpringBoard SIGSEGV** (CFDataGetBytePtr(NULL) in -[CHSWidgetExtensionsBox initWithCoder:]): `containsValueForKey:
  @"data"` was true but the NSData decoded as nil. Cause: stale memory entries. libxpc sends xpc_data > 16 KB as
  memory entries (mach_make_memory_entry_64 + mach_vm_map); darlingserver reused an entry whose range covered the
  address although the process had since unmapped it and malloc() had reused the address for private memory, so the
  receiver got the *previous* memfd's bytes. Probes tests/probes/xdatprobe.c (raw xpc_data 16-32 KB: wrong bytes) and
  nsdprobe.m (NSXPC NSData 16384-20000: the previous message's object) failed on Darling, pass on the Mac and now.
  Fix (duct-tape memory.c dtape_share_region): /proc/<pid>/maps decides. A MAP_SHARED mapping of one of our memfds is
  shared as that memfd (descriptors are registered by inode; the maps parser in process.cpp now returns the inode);
  for private memory an existing entry is only reused if the memfd still holds the same bytes, otherwise the receiver
  gets a fresh snapshot (no remap under the process). Re-sharing private regions afresh (remap) was tried first and
  works for the probes, but lsd's 6.5 MB store shows up private on every send and was copied/remapped 45 times.
- **Gray icons are not caused by this**: with the fix SpringBoard no longer crashes, and its *first* instance never
  gets colour icons (they only appeared after the old crash restarted SpringBoard). Open.
- **fileport_makeport/fileport_makefd** (NSFileHandle over NSXPC, xpc_fd_create) hit the old LKM API (`lkm_call` ->
  __builtin_unreachable, SIGILL in chronod). Now darlingserver RPCs (`fileport_makeport` takes the fd, `fileport_makefd`
  returns a dup): duct-tape traps.c keeps the fd as the fileglob of an IKOT_FILEPORT kobject port; no-senders closes it.
  Probe tests/probes/fdprobe.c.
- **statfs f_flags** were Linux ST_* flags copied raw (ST_VALID read as MNT_UNION, MNT_LOCAL missing):
  libsystem_coreservices' _dirhelper_relative_internal refuses non-local volumes with ENODEV (19), so chronod could not
  create its widget timeline/placeholder directories. conversion/stat/common.c converts them (MNT_LOCAL unless
  NFS/SMB/FUSE, MNT_DOVOLFS).
- **Darwin xattr names** ("ArchiveFilePromise_ReloadDate", "com.apple.chrono.icon-version", ...) failed with
  EOPNOTSUPP: Linux only allows "user.*" for unprivileged processes. All xattr calls now map name <-> "user." + name
  (xattr/xattr_names.h; list strips the prefix and hides other namespaces; ENODATA -> ENOATTR). getattrlist's Finder
  info/resource fork lookups use the same names. With this, chronod writes widget archives: **the Calendar widget
  renders** (docs/screens/16-calendar-widget.png). Note: no cross-process IOSurfaces were needed for this; widget
  content goes as archive files (+ fileports), IOSURFACE_REMOTE_EMULATED stays on.
- **PosterBoard** is a LaunchAngel (System/Library/LaunchAngels/com.apple.PosterBoard.plist; on the Mac it runs as
  user/501/com.apple.PosterBoard). SpringBoard's PRSService/PRSWallpaperObserver "failed to lookup endpoint"
  (com.apple.posterboardservices.services). mksimdevice.py now copies jobs found in LaunchAngels into
  device/LaunchAngels and passes that directory to launchd_sim; rundev.sh starts com.apple.PosterBoard. Then:
  - setattrlist ATTR_CMN_FNDRINFO (FSNode setFinderInfo:) returned EINVAL, so PosterBoard could not create poster
    "scratch" directories: setattrlist_generic.c stores Finder info as the com.apple.FinderInfo xattr and accepts
    BKUPTIME/OWNERID/GRPID/FLAGS/DATA_PROTECT_FLAGS.
  - sandbox extensions (`__mac_syscall("Sandbox")`, ENOSYS: "cannot extend access ... sandbox errno=78"): simshim.c
    issues opaque tokens and accepts any token (nothing is sandboxed here).
  - PosterBoard crashed (NSData writeToURL: nil URL): kern.bootsessionuuid was missing, so
    -[NSString pbf_bootInstanceIdentifier] was nil and with it the transient-info URL. sysctl_kern.c returns Linux's
    boot_id (upper case). PosterBoard now stays up and launches ~10 poster extensions.
- Maps widget now shows its (map-coloured) placeholder (docs/screens/17-maps-placeholder.png).

New diagnostics: host/simshim/diag/oslogdiag.m (os_log to data/var/log/oslog.<prog>.<pid>.log for the program names in
data/oslog.filter, formats messages itself; X86CODE_INSERT=host/simshim/diag/liboslogdiag.dylib), errdiag.c (file-system
calls failing with EINVAL/ENOTSUP/ENOSYS, with callers), chsdiag.m (STRPOPACKCoder), pbdiag.m (PosterBoard).

Still open / next:
- **Extensions using BoardServices listeners crash: "Couldn't retrieve XPCService dictionary from service bundle.
  Cause: Process is not managed"** (_xpc_copy_xpcservice_dictionary.cold.2 from xpc_connection_create_bs_service_listener):
  ScreenTimeWidgetExtension, PassbookWidgets (the SIGKILL/SIGILL loops) and CollectionsPoster ("no listener at
  XPCService backstop" for SceneSessionService), i.e. the poster that would draw the default wallpaper. launchd_sim
  apparently does not consider these pid/<host> xpcservice instances managed when libxpc asks for the service
  dictionary. Wallpaper stays black until this works.
- First SpringBoard instance keeps gray icons; Calendar (dynamic) icon not investigated; CalendarWidgetExtension
  SIGSEGV/SIGABRT (widget still renders); Maps widget content needs locationd; IconFoundation cache "Incorrect formed
  data" (layer data size garbage); "copy_read_only: vm_protect failed: status 5" (VM_PROT_COPY?); fsctl (242) ENOSYS.

### Icon shmem, gray first-instance icons, "not managed", wallpaper (2026-10-27, cont.)

- **Wrong icon images (stale memory entry over a file mapping), fixed.** iconservicesagent answers each icon request
  by mmap()ing the icon's `.isdata` cache file (MAP_SHARED), `xpc_shmem_create_readonly()` over it, and unmapping it,
  so every icon sat at the same address. host/simshim/diag/shmdiag.c (X86CODE_INSERT; logs a checksum at
  xpc_shmem_create(_readonly) and xpc_shmem_map) showed SpringBoard receiving the *first* icon's bytes for all of
  them (Safari's icon showed up in the Photos slot). dtape_share_region only checked for stale entries in
  *private* memory; a shared mapping that is not one of our memfds (a file) reused the old entry, and the first time
  it even replaced the agent's file mapping with a memfd. Now (duct-tape memory.c): entries over such a mapping are
  dropped and the receiver gets a snapshot; file mappings are never remapped. After the fix SpringBoard's checksums
  match the agent's (57/57). tests/probes/shmprobe.c (shmem semantics after writes/unmap/reuse) matches macOS.
  Note: anonymous MAP_SHARED regions that are not our memfds now also get snapshots (they were remapped to a
  memfd before), i.e. later writes are no longer shared with the receiver.
- **Gray icons in the first SpringBoard: diagnosed, not fixed.** The icon cache is *expected* to be reset at every
  boot (the agent resets it unless the cache dir is owned by `_iconservices`; the Mac's sim rewrites
  `__cache_info__` on every boot too, checked). So the first SpringBoard always misses ("RETURNING cache miss
  PLACEHOLDER"), and a SpringBoard started later gets synchronous hits (docs/screens/18-icons-after-restart.png:
  killing SpringBoard at 100 s gives all icons in colour). host/simshim/diag/imgdiag.m (SpringBoard: SBIcon
  background-generation blocks, -[SBIcon loadIconLayerInBackgroundWithInfo:...completionHandler:] completions with
  the returned layer's CGImage format/pixel, _notifyImageDidUpdate/reloadIconImage, SBHIconImageCache) shows the
  call pattern is identical in both instances: every background block runs and completes, but for the home-screen
  icons (`retrieveNewContentsLayerView_async`) `makeIconLayerNativelyOrFromImageIfNecessaryWithInfo:` returns a nil
  layer in the first instance, and nothing re-requests the image later (no SBLeafIcon "did generate image"
  notification; calling -[SBApplication _noteIconDataSourceDidChange] on all apps 70 s in did not help either).
  The images that do arrive are RGBA16F (bpc 16, kCGBitmapFloatComponents, "Layer data size: 0"; the agent logs
  "layer data size zero" and "Failed to serialize finalized icon ... TextureIOError.usingSoftwareRenderer", i.e.
  without Metal it cannot produce the icon layer data). Next: find why the async load yields nil (likely the missing
  layer data / software renderer path in IconServices' ISIcon layer creation) and what the Mac's first SpringBoard
  gets there (its sim logs these at debug level only; `log stream` on the host did not show them).
- **"Couldn't retrieve XPCService dictionary ... Process is not managed" no longer reproduces** (several boots,
  oslog of CollectionsPoster/ScreenTime/Passbook extensions: the BoardServices XPCServiceListener activates; no
  SIGILL cores). For reference: libxpc's _fetch_xpcservice_info sends routine 718 {type 6, self true} to
  launchd_sim; the handler (launchd_sim +0x1fab1) requires job->pid == caller pid, then the job's (or its parent
  template's) bundle object, else returns 113 "Process is not managed". Instance jobs
  (pid/<host>/<ext>.<UUID>, "draining messages from <ext>.xpc-session") get their bundle from the template.
- **Wallpaper still black.** CollectionsPoster now runs and stays up (main thread idle in its run loop), but logs
  `no listener at XPCService backstop` for SceneSessionService and com.apple.frontboard.workspace-service (the host's
  scene connection arrives before/without a UIKit scene listener) and "Wallpaper Collection metadata file does not
  exist" for iPhone-D73-D74/Titanium-D83-D84 (runtime has no WallpaperCollection.plist there; probably normal).
  Next: why the poster extension registers no SceneSessionService listener (UIKit extension scene setup,
  `_UIApplicationDelegateClass`/NSExtension main) and PosterBoard's side of the scene.

Other: cli/libexec/rundev.sh (now bash) also keeps all launchd lines except failed lookups in /tmp/rundev-launchd.log
(launchd.log rotates within a minute because of ~35k `com.apple.diagnosticd` lookups from pid-domain extensions,
which do not get OS_ACTIVITY_MODE=disable). Landmines found: darlingserver aborts on mach_vm_region (stub ->
dtape_stub_log abort, kills the whole container); proc_listallpids (proc_info callnum 1) unsupported;
backboardd once SIGILLed in pthread_dependency_wait_np (CoreMotion via LightSourceSupport), not reproduced.
New diagnostics: host/simshim/diag/shmdiag.c, imgdiag.m; tests/probes/shmprobe.c, lcprobe.c (launchctl print from
inside the sim; needs proc_listallpids).

### vm_region, proc_listpids, colour icons on first boot, extension NOTE_EXEC, wallpaper = Metal (2026-10-27, cont.)

- **mach_vm_region / mach_vm_region_recurse** (duct-tape memory.c): all XNU flavors now, from /proc/<pid>/maps of the
  target (BASIC_INFO(_64), EXTENDED_INFO(+__legacy), TOP_INFO; recurse with short and full V0/V1/V2 submap info;
  page statistics approximated, depth always 0, KERN_INVALID_ADDRESS past the last region, [vsyscall] hidden).
  vm_map_region / vm_map_region_recurse_64 (the vm_region(_64)/vm_region_recurse MIG routines) were
  dtape_stub_unsafe aborts that killed the whole container; they forward now. **proc_listpids / proc_listallpids**
  (proc_info callnum 1, xnu emulation proc_info.c): pids from the container's /proc, ALL/PGRP/PPID/TTY/UID/RUID,
  XNU's byte-count returns and NULL-buffer estimate. Probe tests/probes/vmrprobe.c (1774 regions walked both ways,
  every flavor, self in proc_listallpids).
- **Gray icons in the first SpringBoard: fixed** (docs/screens/19-first-boot-colour-icons.png, no restart needed;
  the Calendar app icon also shows its live date). Cause: the home screen loads icon *layers*, not images:
  -[SBLeafIcon makeIconLayerWithInfo:...] -> SBHGetIconLayerWithTraitCollection -> SBHGetIconLayerFromIconServicesImage,
  which calls -[IFConcreteImage ICRIconLayer] and only falls back to a CGImage-backed CALayer when the caller passes
  the image-fallback option (the home screen doesn't). ICRIconLayer needs an RBDevice (Metal) and the image's layer
  data; iconservicesagent's software renderer produces neither ("layer data size zero",
  TextureIOError.usingSoftwareRenderer), so every async load after a cache miss returned nil. A restarted
  SpringBoard only looked fine because cache hits are served synchronously through the UIImage path. Fix
  (host/simshim/compat/simshim.c): when -[IFConcreteImage ICRIconLayer] returns nil, return a CALayer with the flat CGImage
  as contents (contentsScale from the image), inside an explicit background CATransaction when off the main thread
  (otherwise CA aborts: CA_ABORT_ON_NON_MAIN_THREAD_TRANSACTION_PUSH). Found with imgdiag.m hooks (return values of
  makeIconLayerNativelyOrFromImageIfNecessary.../makeIconLayer.../makeIconImage...) and selector names resolved at
  runtime from selref addresses (otool can't resolve the chained-fixup selrefs; lldb can't attach on the Mac).
  The "verify_image_parameters: invalid image colorspace: NULL" errors are unrelated (PosterLegibilityKit label
  images, oslogdiag now prints call stacks for messages matching the first line of data/oslog.bt).
- **Extension processes had no bundle properties in runningboardd: fixed.** Every extension logged "could not find
  extension point for xpcservice<...>", and MercuryPosterExtension (the selected lock/home poster,
  com.apple.MercuryPoster, hosted by SpringBoard, not CollectionsPoster) spun ~200k times through PosterKit's
  "FinishRender" assertions refused with "originator doesn't have extensionPoint com.apple.posterkit.provider".
  RBXPCBundleProperties initWithPID: reads RBSExecutablePathForPID(pid) when launchd_sim reports the job running,
  and got xpcproxy_sim (host/simshim/diag/rbdiag.m). launchd_sim marks the job running on EVFILT_PROC NOTE_EXEC; in
  Darling posix_spawn returns at the Linux execve of mldr and the child registers that exec with darlingserver
  later, after launchd has attached its source, so xpcproxy_sim's own start was reported as the service's exec
  (running ~0.5 ms after SOURCE_ATTACH; on the Mac 100-800 ms later, at the real exec). Fix: new RPC spawn_exec
  (darlingserver generate-rpc-wrappers.py, call.cpp, process.cpp/hpp): posix_spawn's forked child calls it before
  execve and darlingserver doesn't send NOTE_EXEC for that exec (it isn't observable on macOS either);
  SETEXEC execs are still reported. Now "running" comes after the real exec and the assertion loop is gone.
  The earlier "no listener at XPCService backstop" faults for CollectionsPoster are normal: the Mac's sim logs the
  same ones (CollectionsPoster only serves PosterBoard's gallery/descriptor updates).
- **Wallpaper: blocked on Metal.** MercuryPosterExtension now gets its scene, then "couldn't set up coordinator:
  MercuryPosterExtension.WallpaperCoordinatorError.unavailable" right after its lookup of com.apple.metal.simulator
  fails (the host-side Metal service of the simulator; there is no Metal here). Options: a non-Metal default poster
  (e.g. a gradient/photo/LegacyPoster configuration seeded into PosterBoard's data store), or a Metal stand-in.
- SpringBoard's first instance sometimes dies early: SIGILL in dispatch_assert_queue(main) from
  -[SBRecentAppLayoutsPersister initWithPersistenceURL:...] (via -[SBAppSwitcherModel init]) on a non-main thread;
  launchd restarts it and the second instance is fine. Not investigated.
- New diagnostics: host/simshim/diag/rbdiag.m (runningboardd bundle properties), imgdiag.m (layer path return values,
  CGImageCreate with NULL colour space), oslogdiag.m (data/oslog.bt stacks); probes vmrprobe.c, xbprobe.m
  (xpc_bundle_create, RBSExecutablePathForPID/RBSBundleIDForPID for all pids).

## Spike 3.5 — Metal stand-in, stages 1-3 (2026-10-27)

**Milestone:** a simulator process gets a working `MTLCreateSystemDefaultDevice()` backed by the host GPU (RTX 5090,
Vulkan). tests/metal/mtlprobe, run as a launchd job, passes 23/23 checks through the real path: sim Metal ->
MTLSimDriver -> XPC -> **Apple's own MTLSimImplementation** in our `metalhostd` -> our MTLDevice classes (vkmtl.m) ->
Unix socket -> `mtlvk` (Rust, steelbrain/metal2vulkan's runtime) -> Vulkan. Buffers round trip, textures
(replaceRegion/getBytes, also sub-regions), GPU clear of a private BGRA8 render target read back by blit, blits
buffer->buffer / buffer->texture / texture->buffer, fillBuffer, scheduled/completed handlers, waitUntilCompleted,
shared events signalled by the GPU and the CPU, GPU waits on events, notifyListener, 100 command buffers in order
(~1 ms each). Details, build and run: docs/METAL.md (`tests/metal/run-probe.sh`).

- **Opt-in only** (`X86CODE_METAL=1` in mksimdevice/rundev adds the job, registered under
  `com.apple.metal.simulator.mtlprobe`; MTLSimDriver looks up `com.apple.metal.simulator.<progname>` before the generic
  name). The generic name stays unregistered, so backboardd/SpringBoard keep the software renderer. Normal boot checked
  after all changes: home screen identical (docs/screens/20-normal-boot-after-metal-changes.png vs 19).
- **metal2vulkan** (external/metal2vulkan, f6a96b7, GPL-2.0-or-later OR LGPL-3.0-or-later, verified in LICENSE):
  builds with the system Rust 1.99, its examples run on the RTX 5090, and its runtime API mirrors Metal closely
  (Device/Buffer/Texture/CommandBuffer/encoders/SharedEvent), so mtlvk is a thin protocol layer. It imports memfd
  pages zero-copy (`new_buffer_with_bytes_no_copy`, VK_EXT_external_memory_host) on NVIDIA. Its translator is the
  plan for stage 4/5 shaders.
- **sim2host**: MTLSimDriver calls `xpc_connection_enable_sim2host_4sim` on its connections. In the sim's libxpc that
  sets the connection's platform to 2, which packs messages with the macOS magic `XPC!` instead of the simulator's
  `@XPC` (table at libxpc __TEXT,__const 0x36ca0): meant for the Mac-side SimMetalHost. Our service runs on the sim's
  libxpc, and the handshake failed with an XPC error. simshim skips the call for `com.apple.metal.simulator*`.
- **Anonymous shared memory was not shared (darlingserver, fixed).** MTLSimDriver allocates each MTLBuffer and each
  getBytes destination with `mmap(MAP_SHARED|MAP_ANON)` and sends it with `xpc_shmem_create`. Since the icon fix,
  darlingserver handed out *snapshots* of every MAP_SHARED region that isn't one of its memfds (meant for file
  mappings), so the service and the app had separate copies: GPU results never reached the app. Now
  memoryRegionInfo reads the path too, and anonymous shared memory ("/dev/zero (deleted)") is moved into a memfd like
  private memory; file mappings still get snapshots.
- **Darling-private syscall 599** (`x86code_memfd_for_region`, darlingserver RPC + libsystem_kernel): returns a Linux fd
  for the memfd behind a range of the caller's memory (sharing it first). metalhostd passes it to mtlvk with
  SCM_RIGHTS (Darling's sendmsg converts BSD cmsgs, and AF_UNIX paths are vchroot-expanded, so
  `/Volumes/SystemRoot/<path>` reaches a Linux socket). The RPC is appended at the end of the RPC list: inserting it in
  the middle renumbered later calls and darlingserver aborted ("Message buffer was too small") on calls from the
  not-rebuilt mldr/launcher. Install without sudo: only darlingserver and libsystem_kernel.dylib change (copy + rename
  over the busy files).
- MTLSimImplementation gets its device from `MTLCreateSystemDefaultDevice` (MTLSimApplicationContext ctor) and passes it
  to `MTLCreateDeserializerForDevice`; metalhostd rebinds that GOT entry after dlopen (1 entry). The client's families
  and limits are hard-coded in MTLSimDriver (`-[MTLSimDevice featureProfile]` = 1 -> Common1, maxBufferLength 256 MB,
  name "Apple iOS simulator GPU"); only registryID and hostFeatureProfile come from the server.
- The only unimplemented selector the deserializer used in this run was `allowLibrariesFromOtherPlatforms` (now YES).

Next (stage 4, compute): function creation (message 15, serialized metallib + `overrideTargetTriple`), compute
pipelines and encoders in VKMTL/mtlvk on metal2vulkan's translator; first check that it accepts the simulator's AIR
(air64_v28, iOS 26.5 simulator). Then render pipelines/draws (stage 5), IOSurface textures (needs the IOSurface Remote
service) before CA can use it.

### Stage 4: compute (2026-10-27)

**Milestone:** compute shaders written for iOS run in a simulator process on the host GPU. mtlprobe passes 34/34
(the 23 checks of stages 1-3 and 11 compute checks): a metallib compiled by Xcode 27.1 for the iOS 26.5 simulator
(`air64_v28-apple-ios26.5.0-simulator`) loads with `newLibraryWithData`, and so does the same source compiled at
run time with `newLibraryWithSource`. The checks are float vector add, `setBytes` parameters with
`dispatchThreadgroups`, a kernel writing an RGBA8 texture (read back with getBytes), a reduction in threadgroup
memory (`setThreadgroupMemoryLength`), function constants (by index and by name), a sampler with a texture sample,
and a kernel that reads buffer pointers it was given as `gpuAddress` values (`useResource`). The CPU checks every
result. Pipelines report threadExecutionWidth 32 and maxTotalThreadsPerThreadgroup 512 to the client; the client
clamps the runtime's 1024. Details: docs/METAL.md ("Shader path").

- **What the server gets.** The client compiles everything to AIR. MTLCompilerService runs under Darling, so
  `newLibraryWithSource` works too: about 0.5 s cold, then cached. For each function or pipeline the client sends
  one *specialized* function as a small metallib (format 2.4, triple `air64-apple-ios13.0.0-simulator`). Function
  constants are already folded in, so the server never calls `newFunctionWithName:constantValues:`. MTLSimImplementation
  then calls these on our device: `newLibraryWithData:error:`; `setOverrideTriple:@"air64-apple-macosx10.14.0"` on the
  library (meant for the Mac's compiler; we ignore it, because metal2vulkan takes the simulator triple as it is);
  `newFunctionWithName:`; `newComputePipelineDescriptorWithSerializedData:deserializationContext:`; and
  `newComputePipelineStateWithDescriptor:options:reflection:error:`. The descriptor deserializers are methods of
  Metal.framework's `_MTLDevice` and don't touch the device, so VKMTLDevice borrows Apple's IMPs, including the
  render, tile and mesh ones for stage 5. MetalSerializer checks encoder ranges against `maxComputeBuffers` and the
  other per-stage table sizes, and against `maxComputeLocalMemorySizes`, which is an NSArray with one entry per
  threadgroup-memory index. Before the device answered these, the server aborted with "Invalid buffer count" /
  "Invalid threadgroup memory size index".
- **GPU addresses.** `BUF_NEW` replies with the runtime's `Buffer::gpu_address()`, and VKMTLBuffer returns it from
  `gpuAddress`. MTLSimDriver asks the server for it (`bufferGPUAddress`), so pointers written into buffers by the
  app are real device addresses (buffer device address) and kernels dereference them.
- **Translator.** metal2vulkan (f6a96b7) compiled every probe kernel and the captured wire metallibs unchanged.
  The survey tool (`mtlvk/src/bin/survey.rs`) runs over the runtime's metallibs for the RTX 50 profile with
  constants zeroed. The only real failure in Apple's simulator shaders was in MTLSimDriver's `pvrtc4` decoder:
  `select i1 %c, ptr addrspace(2) @standardMod, ptr addrspace(2) @punchthroughMod`, a select between two
  constant-global addresses that live in the pipeline's constant blob. The blob lowering took the select's arms as
  SSA operands and refused constants. Fixed in `lower/access/blob.rs`: the arms are now blob offsets (`LocalPtr`),
  the same way GEP and copy take them. There is a regression test (`tests::engine_arena::a_select_of_constant_globals_selects_their_blob_offsets`,
  which fails without the fix). The full `metal2vulkan` lib test suite passes with it. Patch:
  the metal2vulkan x86code branch. Survey after the fix: MTLSimDriver 12/12, CoreImage 60/60. QuartzCore
  141/168: 13 tile kernels need tile attachments, and 14 framebuffer-fetch fragments need their attachment formats.
  Both are inputs the pipeline creation supplies, not refusals. RenderBox 54/60 (linked `custom_fn`, framebuffer
  fetch), MercuryPoster 36/44. **7 MercuryPoster fragments are refused: MSL 3.2 program-scope buffer bindings
  (`air.global_bindings`) are not implemented in the translator.** Those are wallpaper shaders (stage 5/8).
- Gating unchanged: X86CODE_METAL opt-in, no Darling or simshim changes in this stage. (Only cli/libexec/mksimdevice.py
  changed: it now passes `X86CODE_VKMTL_DUMP` to metalhostd.) The normal boot was checked afterwards, see below.
- Normal boot after stage 4 (no X86CODE_METAL): the home screen is the same as 19 (docs/screens/21-normal-boot-after-stage4.png).

### Stage 5: cross-process IOSurfaces, render pipelines, CoreAnimation on Metal (2026-10-27)

**Milestones:**
- **IOSurfaces are shared across simulator processes.** host/iosurfaced/iosurfaced serves
  `com.apple.IOSurface.Remote`, the service CoreSimulator's SimRenderServer provides on a Mac. It is now part of
  every device (`X86CODE_IOSURFACE`, default 1; `IOSURFACE_REMOTE_EMULATED` is no longer set).
  Two-process probe 14/14: lookup by ID and through `IOSurfaceCreateXPCObject`/mach port, pixels written both ways,
  lock seeds, values, use counts, a planar layout. The normal boot renders the same home screen
  (22-normal-boot-iosurface-remote.png, 25-normal-boot-after-stage5.png vs 19/21).
- **Render pipelines on Vulkan.** mtlprobe 46/46: the 34 stage 1-4 checks plus 12 render checks, each verified on
  the CPU. They cover a clear plus an interpolated triangle, an indexed quad with a vertex descriptor, texture,
  sampler and setFragmentBytes, blending, scissor, a depth test, 4x MSAA with resolve, and rendering into an
  IOSurface texture that a second process reads back through IOSurfaceLookup.
- **CoreAnimation's Metal renderer composites the home screen** (`X86CODE_METAL=backboardd`):
  24-metal-backboardd-home.png. It matches the software renderer except the dock, where Metal draws the iOS 26 glass
  platter that the software renderer leaves out.

Details:
- **iosurfaced** (sim binary, Darling side, launchd job with MachServices `com.apple.IOSurface.Remote`, KeepAlive:
  clients `exit()` when they lose the connection). Surfaces are `mmap(MAP_SHARED|MAP_ANON)` memory sent as
  `MemoryObject` xpc_shmem; darlingserver moves it into a memfd on first share, so every client and mtlvk (syscall
  599) share the pages. Mach ports from CreateMachPort are receive rights we keep, and a lookup by port finds the
  surface by the port's name. Each connection's references are dropped when the connection goes away. The client
  calls `xpc_connection_enable_sim2host_4sim` on this connection too, so simshim skips that for the name. Two
  protocol details were not in PROTOCOL.md: `ErrorCode` must be uint64 (the client uses `get_uint64`), and SetValue
  sends `{"key", "value"}` or `{"isSetValues", "value"}`.
  In the normal boot, 75 surfaces were created, and 2 were passed across processes by mach port (looked up in
  backboardd).
- **Render protocol** (vkmtl_proto.h): `RPS_NEW`, `DSS_NEW`, `TEX_NEW_BUF` and render-encoder commands `C_RE_*`
  (30-49). `C_RENDER_PASS` now opens an encoder, and `C_RE_END` closes it. The Darling side records each encoder
  locally and sends it at `endEncoding`, so late `setColorStoreAction:` calls are included. mtlvk maps everything 1:1
  onto metal2vulkan's runtime (`new_render_pipeline_state`, `RenderCommandEncoder`,
  `Buffer::new_texture_with_iosurface_plane`). That needed about 400 lines in mtlvk and no runtime changes.
- **IOSurface textures** (message 13): MTLSimImplementation looks the surface up (`IOSurfaceLookupFromXPCObject`,
  which works now that the service exists). MetalSerializer then validates it, and its check
  `bytesPerRow & ([device iosurfaceTextureAlignmentBytes] - 1)` failed every surface while the selector was
  unimplemented and answered 0. We answer 16. vkmtl wraps the surface's pages in a buffer (memfd through syscall 599,
  imported without a copy) and makes the texture over it.
- **CoreAnimation on Metal.** backboardd made 27 render pipelines in a 200 s boot and none were refused. They include
  the glass/variable-blur shaders. Its render targets are SimFramebuffer's three swapchain IOSurfaces. It renders
  only each buffer's damaged region, which works because our textures alias the surfaces' pages. The first
  screenshots were missing layers (23-metal-backboardd-unsynced-present.png): CA passes no fence to
  `SFBSwapchainSwapAddSurface` (both extra arguments are 0 in `CA::WindowServer::SimDisplay::finish_update`), and our
  simfb copied the surface at `SwapSubmit` while the GPU was still drawing it. simfb now dumps 40 ms after the
  submit (the latest submit wins). That fixed it (24).
  - It stays **opt-in**. It needs the Linux mtlvk process for the whole boot (`cli/libexec/rundev.sh` starts it when
    X86CODE_METAL is set). Only the static home screen has been checked. Presentation needs a real
    GPU-completion signal instead of the delay.
- **Wallpaper shaders (translator).** MSL 3.2 program-scope bindings (`air.global_bindings`) are implemented in
  metal2vulkan: the ABI decodes them, and a canon pass turns each bound global's loads into an entry parameter at
  the binding's index. MercuryPoster survey 36/44 -> 43/44. The last function needs a framebuffer-fetch format,
  which a pipeline supplies. Patch: the metal2vulkan x86code branch (both fixes, applies to f6a96b7).
  metal2vulkan lib tests pass, except two tests that read a process-wide counter and fail when tests run in
  parallel; they pass with `--test-threads=1`. No wallpaper has been rendered yet: PosterBoard/MercuryPoster would
  need `X86CODE_METAL=...,PosterBoard` or the extension's process name.
- No Darling changes in this stage, so patches/darling-iossim.patch is unchanged. Changed: simshim (sim2host skip),
  simfb (delayed dump), cli/libexec/mksimdevice.py (X86CODE_IOSURFACE, `label=program|arg|mach:name` jobs),
  cli/libexec/rundev.sh (starts mtlvk), build/mac-build-iosurface.sh (new), build/mac-build-metal.sh (links IOSurface).

Next: a soak test of CA on Metal (animations, app launch, touches) before making it the default for backboardd; a
GPU-completion signal for SimFramebuffer presents; the wallpaper (PosterBoard/MercuryPoster on Metal); heaps,
texture views, shared-event handles (message 17), indirect draws.

## Spike 3.6 — GPU-synced presents, touch input, wallpaper, Metal by default (2026-10-27)

**Milestones:**
- **Touch, keyboard and buttons work.** host/simhid is our x86_64 SimulatorHID. Tapping the Settings icon from a
  test opens Settings (26-tap-opens-settings.png). Rows navigate (27), lists scroll with animation (28), and the
  home button returns to the home screen. Page swipes work (29), and hardware-keyboard typing reaches text fields
  (30). cli/libexec/simview.py now sends mouse and keyboard input; cli/libexec/simtouch.py drives the same input from scripts.
- **The default wallpaper renders.** MercuryPosterExtension draws the iOS 26 planet wallpaper on Metal (32).
- **Presents wait for real GPU completion.** The fixed 40 ms delay is gone.
- **Metal is the default** (`cli/libexec/rundev.sh`: `X86CODE_METAL=all` unless set). Settings, Calendar, Safari and Maps
  (its Metal map view, 35) work on it. If mtlvk dies, the boot falls back to software rendering.

### Presents synchronized with the GPU (simfb)
- CA passes no fence. `SFBSwapchainSwapAddSurface`'s two extra arguments are 0. QuartzCore never imports
  `SFBSwapchainAcquireSurfaceFence` (it is one of the 39 symbols only the shim dlsym()s). The real host relies on
  the macOS GPU driver's implicit IOSurface synchronization: its reads of the surface are ordered after the
  simulator's GPU writes. So there is no fence object to forward.
- simfb now tracks completion inside the process. At the first `SwapBegin` after MTLSimDriver is loaded, it
  overrides `-[MTLSimCommandBuffer commit]` (`class_addMethod` on that class only; `commit` is inherited from
  Metal's `_MTLCommandBuffer`). Each commit gets a sequence number and a completed handler. The handler fires when
  metalhostd relays mtlvk's `EV_CB_DONE`, and by then the surface's shared pages hold the result. A present
  records the last committed sequence number at `SwapSubmit`. The surface is copied as soon as every command buffer
  up to that number has completed, with no fixed delay. A newer present supersedes a waiting one. With the
  software renderer nothing is outstanding, so the copy happens at once.
- A watchdog copies a present that has waited more than 1 s (for example when mtlvk is gone) and logs it.
- Measured: CA's Metal frames always waited ("601 presents waited for the GPU, 0 were ready at submit"). So CA
  commits before it submits, as assumed, and no frame has been seen with missing layers.

### SimulatorHID stand-in (host/simhid, touch/keyboard/buttons)
- How the runtime loads it: `SimulatorClient.framework`'s `IndigoHIDSystemSpawnLoopback(IOHIDEventSystemRef)`
  (called by backboardd) does `[NSBundle bundleWithPath:getenv("SIMULATOR_HID_SYSTEM_MANAGER")]`. It takes the
  bundle's `principalClass` (Info.plist `NSPrincipalClass`, `SimHIDSystem` on the Mac), which must conform to
  `SimulatorClientHIDSystemManager`, then calls `-initUsingHIDService:(IOHIDEventSystemRef)error:` and keeps the
  object.
- Apple's version (the Mac's arm64 SimulatorHID, CoreSimulator-1174.9.2) receives Indigo messages on the mach
  port `IndigoHIDRegistrationPort` and dispatches them through virtual HID services in backboardd's event system:
  `_IOHIDServiceCreateVirtual` + `_IOHIDEventSystemAddService` + `_IOHIDServiceDispatchEvent`.
- Ours creates three virtual services: a touchscreen (usage page 0x0D / usage 4, `DisplayIntegrated`, `Built-In`),
  main-screen buttons (consumer page) and a keyboard (generic desktop page 1, usage 6). They are fed from a Unix
  socket instead of Indigo. The callback table was read from the runtime's IOKit: version 2, 0x58 bytes, with
  open/close/copyProperty/setProperty/setEventCallback/schedule/unschedule/copyEvent/setOutputEvent/
  copyMatchingEvent at +8..+0x50. A touch is a digitizer hand event with one finger child: x and y normalized
  0..1, `IsDisplayIntegrated` set, the range/touch/identity mask on down and up, position otherwise. Keys use
  `IOHIDEventCreateKeyboardEvent` (page 7). Home is consumer 0x40, lock 0x30, volume 0xE9/0xEA.
- Socket: `<device data>/simhid.sock` (backboardd binds it through Darling's path translation; Linux clients
  connect directly). Text lines: `touch down|move|up x y`, `key down|up usage`, `button home|lock|volup|voldown
  down|up`. mksimdevice sets `SIMULATOR_HID_SYSTEM_MANAGER` (`X86CODE_HID=0` leaves it out). Build:
  build/mac-build-simhid.sh.
- **Darling fix:** `accept(fd, NULL, NULL)` crashed. libsystem_kernel's accept fixed up the returned sockaddr
  without a NULL check, and backboardd's listener thread crashed on the first client.
- **Darling fix: `__work_interval_ctl` (BSD syscall 499)** was ENOSYS. UIKit creates a work interval for its
  scroll/animation frame pacing and throws "Failed to create work interval" when that fails, so every app aborted
  on its first scroll. Now every operation succeeds and CREATE2 hands out ids (no port).
- **Darling fix: shared `mach_vm_remap`** ignored the target address and flags (it always mapped anywhere). It
  also aborted ("Cannot complexly remap existing shared regions yet") when the source was partly shared already.
  SwiftUI's AttributeGraph grows its arena with `mmap(4x)` + `vm_remap(&new, old size, VM_FLAGS_OVERWRITE, old)`
  and keeps `new` as its base. The new region's head stayed unmapped, so Settings' search crashed in
  `AG::data::table::alloc_page`. It is rewritten on `dtape_share_region` and honours
  ANYWHERE/FIXED/OVERWRITE.
- Simulator.app's other HID devices (trackpad/mouse, game pad, digital crown, CarPlay, dynamic services from
  `/var/run/SimHIDDynamicServices.plist`) are not implemented. Pinch (two fingers) is not implemented either.

### Wallpaper (MercuryPoster on Metal)
- With `X86CODE_METAL=...,MercuryPosterExtension,PosterBoard` the poster gets a device. It decodes its ASTC
  textures with MTLSimDriver's `astc` compute kernel, then renders the `space::*` cylinder/arc/background
  pipelines and `VariableBlurrer`.
- **Texture views** (`newTextureViewWithPixelFormat:` with and without `textureType:levels:slices:` and
  `swizzle:`) were unimplemented, which left the planet black (31). They are implemented now:
  `VKMTL_TEX_VIEW` = 22 maps to the runtime's `new_texture_view_with_levels_slices` / `_with_swizzle`, and
  `parentTexture`/`parentRelativeLevel`/`Slice` report the parent.
- **Darling fix: commpage cache-line bits.** MercuryPosterExtension crash-looped every ~8 s with a Swift trap in
  `vImage_Buffer.createCGImage` while making its snapshot. vImageCreateCGImageFromBuffer failed with
  kvImageMemoryAllocationError. A diagnostic shim (host/simshim/diag/mmapdiag.c) showed
  `mmap(NULL, 0, ..., MAP_SHARED|MAP_ANON, VM_MAKE_TAG(75))` failing with EINVAL. vImageBuffer_Init aligns rowBytes
  to twice the cache-line size, which it reads from `_COMM_PAGE_CPU_CAPABILITIES & 0x70`. Darling's mldr never set
  kCache32/64/128, so every rowBytes was 0. mldr now sets kCache64. This affects any vImage user, not only the
  wallpaper.
- Result: 32-metal-wallpaper-mercury.png. Visible differences from a Mac: the blue disc has speckle ("stars") and
  a magenta rim, probably precision or ASTC-decode differences. These are not investigated yet.

### Metal by default, robustness
- `cli/libexec/rundev.sh` defaults to `X86CODE_METAL=all`. It starts mtlvk first and waits up to 5 s for its socket. If
  mtlvk does not come up, it boots with `X86CODE_METAL=0` (software) and says so on stderr.
- Tested with `all`: Settings (navigate, scroll, search with typing), Calendar, Safari, Maps (onboarding, then its
  Metal map view, 35), home button and page swipes. Three rounds of these launch/scroll/swipe cycles plus several
  minutes idle showed no Metal-related crash. Photos aborts (NSRangeException, empty array) **with the software
  renderer too**, so that crash is not Metal's.
- **If mtlvk dies:** metalhostd's reader sees the socket close and exits (from the main queue: an `_exit()` from
  the reader thread while other threads were inside darlingserver calls made darlingserver abort in
  `Thread::setPendingCall` and took the whole simulator down). Every Metal client then gets
  `XPC_ERROR_CONNECTION_INTERRUPTED`, and MTLSimDriver aborts it by design ("Connection to SimMetalHost ... was
  lost", abort_with_payload). launchd restarts backboardd and SpringBoard. The new metalhostd finds no mtlvk and
  stays in **refusing** mode: it holds the service names and cancels every client connection, so MTLSimDriver
  reports no device and CA/apps render in software. The home screen came back in software about 20 s later, with
  a black wallpaper and no glass dock, and input still worked. When mtlvk is back (metalhostd probes every 10 s),
  metalhostd exits so that the next lookup gets a serving instance. Processes already running in software stay in
  software until they restart.
- Pitfall while testing: a still-running earlier `cli/libexec/rundev.sh` (long SECS) tears down the container when its
  timeout expires, even if a newer boot is running. Kill stale rundev/timeout processes before a new boot.
- Known regression with Metal: Settings' row icons are gray placeholders whenever backboardd composites with Metal.
  They are coloured with the software renderer. Not investigated.

Darling changes in this stage (patches/darling-iossim.patch regenerated): accept NULL address, work_interval_ctl,
shared vm_remap, commpage kCache64 (mldr). Installed without sudo by copying libsystem_kernel.dylib, darlingserver
and mldr next to the old files and renaming them over.

## Spike 3.7 — wallpaper fixed, RenderBox paths, Photos, two-finger touch (2026-10-28)

**Results:**
- **The default wallpaper now matches a Mac** (36, and 37 = our disc next to the Mac mini's iPhone 17 Pro / iOS 26.5
  screenshot, research/macref/home-iphone17pro-ios26.5.png). The speckle and the magenta rim are gone.
- **Photos opens** ("No Photos or Videos", 38). It needed assetsd.
- **Two-finger gestures**: `touch2` in host/simhid. simview: Ctrl+drag pinches, Shift+drag pans with two fingers.
  simtouch: `pinch`, `pan2`. Pinch zooms Maps' map in and out (39).
- RenderBox's `path_interior_fragment` pipelines compile now (68 had been refused per boot).
- Settings' row icons: **not a Metal problem** (see below). Still gray.

### Wallpaper speckle/magenta: misaligned `uint4` loads (translator)
- Found by dumping passes. mtlvk got `MTLVK_DUMP_PASSES=<dir>`, which copies every render pass's colour attachments
  (the resolve texture for MSAA) and every texture a compute encoder bound (all levels) out after the encoder. It
  also logs compute buffer bindings with their first words. MercuryPoster draws one 4x MSAA pass per frame. The
  artefacts were already in that pass. One of the textures it samples, a 1024x1024 RGBA8 texture decoded by
  MTLSimDriver's `astc` kernel, was **magenta with speckles**: magenta is the ASTC error colour, and the speckles
  were the few blocks that happened to decode.
- That texture is mip 1 of a 6x6-block ASTC KTX. The client binds the KTX data at each mip's offset, and those
  offsets are only 4-byte aligned (272, then +1871700, +2339560: KTX puts a 4-byte imageSize before every level).
  The kernel loads blocks as `load <4 x i32>, align 16`. metal2vulkan turned that load into one `uvec4` unit of a
  `uvec4[]` view at index `(residual + m) / 16`, which trusts AIR's alignment. With a bind residual of 4 or 8 that
  read the wrong 16 bytes. Apple GPUs load at any 4-aligned address, so it works on a Mac.
- Fix (the metal2vulkan x86code branch, `memory/leaf.rs`): 16-byte leaves no longer use `uvec4` units
  (`VEC4_UNITS = false`). They go through the aligned four-word path, which is right at any 4-aligned offset.
  Five translator tests check the `uvec4` path or golden hashes. They are now `#[ignore]` with the reason. The
  other 3743 lib tests pass. mtlprobe still passes 46/46.
- Test tool: `mtlvk/src/bin/astctest.rs` runs the `astc` kernel over a region of a KTX, with ASTC partition tables
  built from the spec's hash and a configurable buffer offset (`INPUT_OFFSET`). `tools/debug/astccmp.py` compares the
  result with a CPU decoder (texture2ddecoder, pip, in a venv). The checks: offset 0: 0 of 65536 texels wrong;
  offsets 4 and 8: every block wrong before the fix, 0 after.
- ASTC itself was decoded correctly all along. The `colors portrait home.ktx` region matched the CPU decoder
  exactly before the fix.

### RenderBox `path_interior_fragment` (translator)
- Refused with `Lowering(StageIo) NotImplemented` at argument 0. The function as MTLSimDriver sends it (already
  specialized) takes `[[front_facing]]` as an `i8` (`bool` in memory), and stage IO only accepted `i1`.
  `plan::input_builtin` now accepts an integer form for `FrontFacing`, and `to_lowered` widens the builtin's bool to
  1/0. mtlvk writes a refused render pipeline's metallibs to `var/metal/failed/<id>-{vert,frag}.metallib` +
  `<id>.txt`; `mtlvk/src/bin/rpsrepro.rs` replays them through the compiler (`rpsrepro <profile> vert frag
  I:FORMAT... stencil=N`).
- 0 refused pipelines in the boots after the fix.

### Settings row icons are gray with the software renderer too (not Metal)
- The Spike 3.6 note was wrong. With `X86CODE_METAL=0` they are gray as well, and 26-tap-opens-settings.png
  (software era) already shows gray squares. The gray squares are Settings' own 84x84 placeholder images. They were
  dumped from replaceRegion uploads (`MTLVK_DUMP_REPLACE=<dir>`).
- iconservicesagent renders the real icons correctly. In the IconsCache, the 87x87 `.isdata` units for
  `com.apple.graphic-icon.gear` etc. are the coloured icons, and the index points to them.
- In Settings (diagnostic hooks, since removed): `-[ISConcreteIcon imageForDescriptor:]` returns `IFPlaceholderImage`
  and `_imageFromStoreForDescriptor:` returns nil for every row, even though `store.index` (mapped r--s, same inode,
  entry present) holds the digest. `getImageForImageDescriptor:completion:` then delivers real CGImage-backed
  `IFImage`s, but the cells never show them, and scrolling does not reload them. Next: why the in-process store
  lookup misses (validation token / descriptor size?) and what PSTableCell's completion block checks.
- `copy_read_only: vm_protect failed: status 5` (73 per boot) comes from CoreGraphics in iconservicesagent. It is a
  Darling vm_protect gap and harmless here.

### Photos: assetsd
- Photos aborted with NSRangeException in `-[PXPhotosDataSource assetCollectionForSection:]`. The real error came
  earlier: `com.apple.photos.service` lookup failed (error 3), so the library never opened ("assetsd could have
  crashed"). `cli/libexec/rundev.sh` now starts `com.apple.assetsd`. Photos opens with an empty library. A Mac's
  simulator has sample photos (CoreSimulator adds them), so ours has none yet.
- simshim can now log apps: list process names in `~/.darling/private/var/tmp/x86code-stderr-progs`. Those
  processes write stderr and os_log (DT mode) to `.../x86code-<name>.log`. Off by default. `cli/libexec/mksimdevice.py`:
  `X86CODE_NODT=all,-<label>` keeps DT mode for one job.

### Multi-touch
- simhid `touch2 down|move|up x1 y1 x2 y2`: one hand event with two finger children (index 1 and 2, own identities,
  centroid on the hand). A new contact lifts the previous one first. simview: Ctrl+drag = pinch about the window
  centre (second finger mirrored through it); Shift+drag = two-finger pan. simtouch: `pinch cx cy r1 r2 [ms]`, `pan2
  x1 y1 x2 y2 [ms]`. Verified in Maps (39): zoom in brings up the scale bar, zoom out shows the denser grid.

### Networking, idle crash loops (looked at only)
- Maps says "No Internet Connection". DNS fails: dnssd connects to `/var/run/mDNSResponder` (on a Mac the simulator
  uses the host's socket). There is none in our root, so every lookup gives -1003. `PF_ROUTE` sockets are EINVAL in
  Darling (`nw_route_get_ifindex`). Both need work: an mDNSResponder stand-in or a forwarder to the host's resolver,
  and routing sockets.
- Idle boot with Settings/Photos/Maps: no crash loops in /tmp/rundev-spawns.log. Only widget extensions were
  SIGKILLed by runningboard (normal).

No Darling changes in this spike (patches/darling-iossim.patch unchanged). Changed: metal2vulkan (patch
regenerated), mtlvk (diagnostics, astctest, rpsrepro), simshim (app stderr opt-in), simhid (touch2), cli/libexec/simview.py,
cli/libexec/simtouch.py, cli/libexec/rundev.sh (assetsd), cli/libexec/mksimdevice.py (NODT exclusions), tools/debug/astccmp.py (new).

## Spike 3.8 — networking (DNS, routes, SystemConfiguration, TCP), Settings icons (2026-10-28)

**Results:**
- **The simulator is online.** A probe inside it (tests/probes/netprobe.m, run as a job) resolves apple.com through
  getaddrinfo and DNSServiceGetAddrInfo (real TTLs, A + AAAA, interface enp15s0). PF_ROUTE RTM_GET answers with the
  Linux route. SCNetworkReachability reports reachable, nw_path_monitor reports `satisfied, interface: enp15s0`. An
  NSURLSession GET of https://www.apple.com/ returns **HTTP 200, 254057 bytes in 0.7-4.8 s** (5 of 5 runs after the last libkqueue fix).
- The simulator's dynamic store now has State:/Network/Global/IPv4 (PrimaryInterface enp15s0, router), Global/DNS
  and the service/interface keys. The status bar shows the Wi-Fi symbol.
- **Maps loads its map tiles, imagery and "Guides We Love" over the network** (40). Its "No Internet Connection"
  banner is still shown (open, see below).
- **Safari doesn't load pages yet** (41: blank page, no error). WebKit never launches its WebContent process. That
  process is an ExtensionKit extension (System/Library/ExtensionKit/Extensions/WebContentExtension.appex), and
  Safari logs `_EXDiscoveryController ... Cannot run query`. This is extension discovery, not networking.
- **Settings row icons:** colour on a second launch of Settings (42). On the first launch they stay gray (open, see below).

Prior work checked first, as asked:
- willi/darling-xnu#7 (PF_ROUTE), #8 (EVFILT_SOCK socket state, SO_NOSIGPIPE/SIGPIPE, SO_NREAD) and #25 (TCP_INFO,
  socket() of unsupported domains → EAFNOSUPPORT), and willi/darling-libkqueue#5 (EVFILT_SOCK) are ported. They were
  written for this runtime (SuperMe's NSURLSession on "taffy"). These notes don't say how willi resolved DNS.
- Web: darlinghq/darling has nothing about running an mDNSResponder; Darling's own getaddrinfo uses libresolv in
  the process. VibeDarling/darling #1095 and #1200 are res_query problems in that path, not in ours. I found no existing server
  that speaks Apple's dnssd_ipc protocol on Linux. Andrew Tunnell-Jones'
  "mDNSResponder IPC Protocol" post and Apple's mDNSShared/dnssd_ipc.h + dnssd_clientstub.c (mDNSResponder main)
  were the reference. The alternative would be Apple's mdnsPosix daemon built for Linux. It does real mDNS too, but
  it needs root for port 5353, so it wasn't used.

### DNS: an mDNSResponder stand-in (linux/mdnsd/mdnsresponder.py)
- The runtime's libsystem_dnssd (mDNSResponder-2881.120.11) connects to `/var/run/mDNSResponder` (overridable with
  DNSSD_UDS_PATH). Network.framework (nw_resolver), CFNetwork and libinfo all resolve through it. On a Mac that path
  reaches the host's daemon. Here a Linux process plays the host daemon and answers from Linux DNS (dnspython, the
  host resolv.conf), with /etc/hosts and nsswitch as fallback for A/AAAA.
- Protocol (dnssd_ipc.h VERSION 1). A request is a 28-byte header (big-endian except client_context, which is
  echoed) followed by data. A request on its own connection gets its 4-byte error on that connection. Subordinate
  requests of a shared connection, and record operations, send a socketpair end by SCM_RIGHTS with the last data
  byte. Their data starts with an empty control-path string, and the error goes to that descriptor. Darling's
  sendmsg passes SCM_RIGHTS through to Linux unchanged. Replies carry flags, interface, error, then the
  operation's fields.
- Answered: GetAddrInfo (A/AAAA with kDNSServiceFlagsAdd; negative answers only with ReturnIntermediates/Timeout,
  as the daemon does), QueryRecord (any type: owner name, type, raw rdata, TTL, CNAMEs with ReturnIntermediates),
  GetProperty (DaemonVersion), GetPID, cancel. Accepted without results: Register (the reply confirms the name),
  Register/Add/Update/Remove record, Browse, Resolve, EnumerateDomains ("local."), Reconfirm, SetDomain, NAT-PMP
  (answers "unsupported"). TLVs are ignored.
- **Socket placement:** the Darling prefix's `/var/run` is `~/.darling/var/run` (not private/var/run). It is an overlay that
  Darling's startup recreates. A socket made there from Linux was invisible inside, and later gave "Stale NFS file
  handle". The daemon now listens on `var/run/mDNSResponder`. cli/libexec/run-launchd-sim.sh links the
  container's `/var/run/mDNSResponder` to `/Volumes/SystemRoot/...` after the container starts, and Darling resolves
  the link inside its vchroot. cli/libexec/rundev.sh starts the daemon (`X86CODE_MDNS=0` leaves it out,
  `X86CODE_MDNS_LOG=1` logs requests to /tmp/mdnsresponder.log).

### Routing sockets (Darling, willi/darling-xnu#7 ported)
- `socket(PF_ROUTE)` was EINVAL, so nw_path evaluation found no route. #7 applied with 3 rejected hunks, fixed by
  hand. Our fork.c has no signal_knotes, and our sendto had no local_dgram path. `LINUX_SOCK_DGRAM` is defined
  locally. The NET_RT_IFLIST/rtnetlink and madvise parts came along. Probe: `RTM_GET 17.253.144.10` → rtm_index 2
  (enp15s0).

### SystemConfiguration: the host's network state (host/netconfig)
- configd_sim's SimulatorSupport plugin (configd/Plugins/SimulatorSupport/simulator_support.c) mirrors the host
  configd's `Setup:/Network/Global/*` and `Setup:`/`State:` `/Network/Service/*/*` and `/Network/Interface/*/*`.
  The simulator's IPMonitor derives Global/IPv4, DNS, the nwi state and dnsinfo from those keys. Our host configd
  (com.x86code.host-configd) was empty.
- `linux/hostnet/hostnet.py` (Linux, run by cli/libexec/mksimdevice.py) writes the keys a Mac's configd would publish
  for the default-route interface: service order, Interface (Ethernet), IPv4/IPv6 addresses, router, and DNS from
  resolv.conf. The job `com.x86code.netconfig` (host/netconfig/netconfig.m, a simulator tool) loads them with
  `SCDynamicStoreCreateWithOptions(kSCDynamicStoreUseHostServer)`. SCD_SERVER can't select a store: release
  SystemConfiguration refuses it (SCDOpen.c: `SCD_SERVER` set → BOOTSTRAP_UNKNOWN_SERVICE, SCError 2002). It
  retries for 30 s because the store's job may not be loaded yet. `netconfig -dump [s]` lists the simulator's
  keys. `X86CODE_NETCONFIG=0` leaves it out.

### TCP connections from Network.framework (Darling: willi #8/#25 + libkqueue#5 ported, and new fixes)
Each of these was found with strace on the probe (sudo, attached by pid). In order:
1. **EVFILT_SOCK** registration was ENOSYS. Ported libkqueue#5 and the xnu#8 pieces it needs (sostate, sockutil,
   nosigpipe, socktables, sigsafe_lock; MSG_NOSIGNAL + XNU's SIGPIPE on sends). Adaptations: our knote's
   `kn_extra_buffer` is a pointer (EVFILT_MACHPORT mallocs it), so the EVFILT_SOCK state is allocated with the knote.
   `kqueue_socket_changed` reaches libkqueue_sim through a FOREIGN_HOOK (`__darling_sim_kqueue_socket_changed`).
   GLOB-listed sources need a CMake re-run (touch CMakeLists.txt). ulock_wait's sigsafe_lock refactor was left out.
2. **IP_BOUND_IF / IPV6_BOUND_IF** were ENOTSUP. They are now Linux `SO_BINDTOIFINDEX` (set and get).
3. **change_fdguard_np (444)** was ENOSYS ("Failed to guard socket fd"). It now validates the fd and applies the fdflags exchange
   (Darling enforces no guards). TCP_INFO/TCP_CONNECTION_INFO come from #25.
4. **The cause of every `-1009 / ENETDOWN (1:50)`: EV_EOF on a socket that was never connected.** Network.framework
   registers EVFILT_READ (EV_DISPATCH) and EVFILT_WRITE before connect(). Linux's epoll reports a new TCP socket as
   EPOLLHUP (read) or EPOLLOUT|EPOLLHUP (write) because it is in TCP_CLOSE. libkqueue turned that into EV_EOF, and
   Network failed the flow at once ("already failing"). XNU's soreadable()/sowriteable() are false for such a
   socket. Now libkqueue (read.c, write.c) drops a hang-up event when the socket has no state yet (sostate: not
   connected, no error). It also drops a stale one: epoll_wait took the event before connect(), and by copyout the
   socket no longer hangs up. With EPOLLONESHOT (EV_DISPATCH) the dropped event had disarmed the descriptor, and the
   program never re-enables an event it never saw. The response then never woke the reader: the TLS handshake and
   request went out, and the read sat 50 s. Such a knote is re-armed edge-triggered.
5. **EVFILT_WRITE data** was TIOCOUTQ, the bytes queued (0 on an idle socket). XNU reports the free space. It is
   now SO_SNDBUF minus the queued bytes (at least 1).
Remaining from willi's list: TCP_NOTSENT_LOWAT, OUT_IF and SIOCGCONNINFO still fail and are logged, and
SO_CONNECTION_IDLE is EINVAL. Network tolerates them.

### Services added to the standard boot
- `com.apple.geod` (Maps' network: tiles and search; 2062 failed lookups per boot before) and `com.apple.nsurlsessiond`.

### Maps banner, Safari (open)
- Maps' banner comes from GeoServices' GEONetworkObserver (an in-process `nw_path_evaluator`). In that boot (all
  fixes in), two of Maps' own early NSURLSession downloads (the `cdn.apple-mapkit.com` web bundle, 6 s after launch)
  still failed with -1009 (1:50), with no "already failing" line. So some ENETDOWN path is left. Maps itself opens
  no TCP sockets (strace): its tiles come through geod and load. Next: strace one of those failing Maps
  downloads.
- Safari: see Results. Next step: why ExtensionKit discovery can't run WebKit's query (LaunchServices registration
  of /System/Library/ExtensionKit/Extensions), and `com.apple.appprotectiond.read` is missing.

### Settings row icons: the store works; first-launch async images are not shown (open)
- Diagnostic shim host/simshim/diag/isdiag.m (X86CODE_INSERT=host/simshim/diag/libisdiag.dylib, ISDIAG_PROGS) hooks the
  lookup chain: `_imageFromStoreForDescriptor:` → `-[ISIconCache findStoreUnitForIcon:...]` → `ISStoreIndex
  enumerateValuesForUUID:bock:` → `-[NSData _ISStoreIndex_nodeRefForKey:]`. The index is a hash table: a 20-byte
  header (version 11, count, 4000 buckets), 8-byte bucket refs (offset relative to the nodes, size in the high
  word), and nodes `{next, uuid[16], value, flags}`. The bucket is `hash64 % 4000`.
- The memory mapping is **not** the problem. Both iconservicesagent (rw-s) and Settings (r--s) map the same inode. Settings'
  mapping matched the file byte for byte, and read through /proc/<pid>/mem later it shows the agent's new entries.
  On the first launch the icons simply aren't in the store yet: the header count was 55 then, and 67 after the
  agent generated Settings' icons. So `_imageFromStoreForDescriptor:` returns nil correctly.
- Settings' rows use `_IconServices_SwiftUI`'s `AsyncIconImage`. It calls `-[ISConcreteIcon
  getImageForImageDescriptor:completion:]` once per icon from a global queue. Every completion arrives 1.1-2.9 s
  later with a real CGImage, but the view never shows it. **On a second launch** (same boot, the cache warm) the
  store lookups hit (bucket 634 → node) and every row is in colour (42). Open: why AsyncIconImage's continuation
  result doesn't reach the view (Swift task cancellation/MainActor hop), and why the agent's store loses entries
  between boots (67 → 55: Settings' icons were gone after the next boot).
- `assessValidationToken:` is never reached in Settings, because the lookups miss before validation.

Darling changes in this spike (patches/darling-iossim.patch regenerated): PF_ROUTE (#7), EVFILT_SOCK + socket state
+ SIGPIPE/SO_NOSIGPIPE/SO_NREAD (#8, libkqueue#5), TCP_INFO/EAFNOSUPPORT (#25), IP_BOUND_IF, change_fdguard_np,
libkqueue's unconnected-socket hang-up and EVFILT_WRITE space. Installed without sudo: libsystem_kernel.dylib,
libkqueue_sim.dylib (then tools/debug/set_macho_platform.py) and libsystem_c.dylib (Darling's libkqueue lives in it).
New: linux/mdnsd, host/netconfig, tests/probes/netprobe.m, host/simshim/diag/isdiag.m. Changed: cli/libexec/rundev.sh (mDNS,
geod, nsurlsessiond), cli/libexec/run-launchd-sim.sh (/var/run/mDNSResponder link), cli/libexec/mksimdevice.py (netconfig job,
`env:K=V` for label=program jobs).

## Spike 3.9 — Swift concurrency, timers, ExtensionKit/WebKit processes, LaunchServices entitlements (2026-10-28)

**Results:**
- **Swift concurrency works.** `Task.detached`, `async let`, task groups, `Task.sleep` and continuations resumed from
  GCD never ran before: the cooperative thread pool was not implemented. tests/probes/taskprobe.swift now completes
  every step in about 0.2 s.
- **Dispatch timers fire on time.** A short `dispatch_after` that was armed while a longer one was pending fired when
  the longer one was due. The same applied to `Task.sleep`, CFRunLoop timers and timed waits.
- **Safari's WebKit processes launch.** The WebContent, Networking and GPU ExtensionKit extensions start, and the GPU
  process stays up. `+[BE*Process ...WithBundleID:...]` now completes with a process instead of never calling back.
  Pages still don't render (open, see below).
- **LaunchServices records carry entitlements again.** lsd had registered every app and extension without them.
  This is what ExtensionKit's launch check reads.
- **A rebuilt LaunchServices database works.** lsd spun forever when the csstore was deleted. It is now written in about 30 s.
- **No more background CPU burn.** Spotlight and the Maps widget had spun at 50-100% CPU and flooded launchd_sim with
  ~45,000 failed lookups. Typing reaches text fields (kbd).
- **Settings row icons:** most rows show colour on first launch now (43). Some rows only update after a scroll (open).

Prior work checked, as asked: research/willi (darling-xnu#26 ported, darlingserver#7/#14 read), the VibeDarling org
(darling-libkqueue#1 is the general form of the timer fix; darling-xnu#15 ported), and a haiku search of VibeDarling
PRs/issues for the cooperative pool, S2C replies, entitlements and the socket options (nothing found).

### Swift's cooperative pool (Darling: libpthread + libsystem_kernel workqueue)
- Probe (tests/probes/taskprobe.swift, run as a job: `taskprobe=tests/probes/taskprobe|env:TASKPROBE_SECS=25`): a
  detached task never ran, while `Task { @MainActor in }` did.
- Swift's global executor enqueues on libdispatch's *cooperative* root queues. libdispatch asks for their threads
  with `_pthread_workqueue_add_cooperativethreads`, and Darling's simcompat.c turned that into a request for
  ordinary threads. Those threads drained the plain root queue of that QoS, so the cooperative queue was never served.
- Fix, as XNU does it (bsd/pthread/pthread_workqueue.c, `workq_reqthreads(..., cooperative)`):
  - libpthread requests cooperative threads with `WQOPS_QUEUE_REQTHREADS2` (0x30).
  - libsystem_kernel's workqueue starts them with `WQ_FLAG_THREAD_COOPERATIVE` (0x01000000), and a parked
    cooperative thread keeps the flag.
  - `_pthread_wqthread_priority()` turns the flag into `_PTHREAD_PRIORITY_COOPERATIVE_FLAG` (0x08000000) for
    libdispatch's worker.
- This was also why WebKit never got its processes: BrowserEngineKit's completion-handler API is a Swift async
  function underneath.

### libkqueue: EV_ADD|EV_ENABLE on an existing timer kept the old deadline
- Seen in the probe after the pool fix: a 0.2 s `asyncAfter` fired after 25 s, exactly when the main RunLoop's
  timeout ended. libdispatch keeps one EVFILT_TIMER knote per clock/QoS bucket. It moves the deadline with
  `EV_ADD|EV_ENABLE|EV_ONESHOT`, `NOTE_ABSOLUTE|NOTE_MACHTIME|NOTE_LEEWAY`. libkqueue's `kevent_copyin_one` took
  the EV_ENABLE branch, which re-armed the timerfd with the knote's *old* data.
- Fix (src/common/kevent.c): EV_ADD on an existing timer goes through `kn_modify` (XNU's filt_timertouch). It is
  scoped to EVFILT_TIMER, to avoid changing the socket filters tuned in Spike 3.8. VibeDarling/darling-libkqueue#1
  found the same bug and fixed it for every filter, with read/write adjustments. That would be the general port if
  other filters need it.

### libkqueue: close() clobbered errno (LaunchServices had no entitlements)
- lsd logged `copy_entitlements_for_path: Unable to load entitlements from ".../MobileSMS": (-1) No such file or
  directory` for every bundle. InstallCoordination's `_IXCopyEntitlementsForPath` calls libsystem_sim_kernel's
  `_sim_load_entitlements_blob(path, buf, 8)` first. That parses `__TEXT,__entitlements` from the first fat slice,
  and `-1/ERANGE` is meant to tell the caller the size. Our probe (tests/probes/entblobprobe.c) got ERANGE and the
  plist, but lsd saw ENOENT.
- Cause: the function closes its fd after setting errno. libkqueue's `kqueue_closed_fd()` hook runs EV_DELETE on
  every kqueue (ENOENT) and saved errno without restoring it. This is the same mechanism as Spike 3.4's simshim
  csops_audittoken workaround.
- Fix: restore errno. After a database rebuild, Safari's record has its entitlements and WebKit's extensions have
  `com.apple.private.extensionkit.host-requirement-exemption`, as on the Mac (tests/probes/wkexprobe.m run there
  with `xcrun simctl spawn` and here). runningboardd's "Host is missing entitlement:
  com.apple.developer.web-browser-engine.host" is gone. ExtensionFoundation's `_configureLaunchContext` skips the
  host check when the extension has that exemption.
- `Cannot run query _EXQuery` in Safari's log is harmless. The Mac logs it too (the com.apple.Safari.web-extension
  point doesn't exist).

### getattrlist: walking to the volume root never ended (lsd rebuild)
- With the csstore deleted, lsd sat at 100% CPU for 15+ minutes. strace showed it alternating `"/home"` and
  `".."`. CoreServicesInternal walks ATTR_CMN_PARENTID upwards.
  - Our parent of a mount root came from the other file system: btrfs subvolume roots are all inode 256, so "/"
    looked like /home's own ID.
  - The prefix root's parent was the Linux directory holding the prefix.
- Fix (getattrlist_generic.c):
  - A directory whose ".." is on another device, or is itself, reports itself as its parent and
    `DIR_MNTSTATUS_MNTPOINT` (ATTR_DIR_MOUNTSTATUS was always 0).
  - The prefix root does the same.

### fsgetpath across processes (WebKit GPU process killed in a loop)
- The GPU process hung in `DownloadCloudDocumentSync` under `+[NSURL URLByResolvingBookmarkData:]`. It was resolving
  Safari's bookmark for WebKit's cache directory, and Safari killed and relaunched it over and over.
- `__CFURLCreateByResolvingBookmarkData` treats a bookmark that doesn't resolve, but whose parent is reachable, as an
  iCloud document that isn't downloaded yet. Ours didn't resolve because fsgetpath only knew the IDs its own process
  had handed out.
- Fix (misc/fsgetpath.c): a table shared by all prefix processes, `/private/var/db/x86code-fileids` (8 MB, mmap'd,
  16384 slots, last writer wins). A hit is only used if the path still has that (dev, ino).
  - willi/darling-xnu#18 solves the same lookup with `open_by_handle_at` + a bounded search. That needs
    CAP_DAC_READ_SEARCH, which our processes don't have.

### ulock_wait2, ULF_NO_ERRNO (Safari: `_dlock_wait` SIGILL)
- BSD syscall 544 (`ulock_wait2`, ns timeout, `ULF_DEADLINE`) was ENOSYS. It is now implemented.
- Ported VibeDarling/darling-xnu#15 (hand-written `___ulock_wait/___ulock_wait2/___ulock_wake` stubs that return the
  raw -errno for `ULF_NO_ERRNO`). The generated stubs turned every error into -1 via cerror, and libdispatch's
  `_dlock_wait` traps on anything but 0, >0, -EINTR, -EFAULT, -ETIMEDOUT and -EOWNERDEAD. So every timed
  `dispatch_group_wait` that timed out killed the process (Safari's `WBSHistoryServiceStore load`).

### darlingserver: one bad S2C reply took everything down
- darlingserver threw "Received S2C reply but thread already had one pending" out of its main loop and every
  process died (SIGILL everywhere).
- Contained, as willi/darlingserver#7 does for protocol errors:
  - The duplicate reply is dropped and logged.
  - `Server::start` catches any exception from `callFromMessage` and drops that message.
- The underlying nesting was fixed by porting willi/darling-xnu#26. The RPC hooks now save and restore the whole
  Linux signal mask, so darlingserver's real-time signals stay blocked inside their own handlers.
- **Still open:** Safari's main thread still gets a duplicate S2C reply (in `mach_make_memory_entry_64` from
  `CA::Render::Shmem::port()`) and then waits for ever. The rest of willi's S2C stack (darlingserver#10/#13/#14,
  which depend on each other) is the next port. #14 doesn't apply to our tree on its own.

### Background CPU burn, typing, Safari's sync agent
- Apps and extensions that SpringBoard/RunningBoard start didn't get `OS_ACTIVITY_MODE=disable` (launchd jobs
  do). Their os_log looked up `com.apple.diagnosticd` for each message. simshim now sets it, unless DT mode is on
  for that process.
- `AFPreferences _languageCodeWithFallback:` ("No language code saved, but Assistant is enabled") looped on the main
  thread in Spotlight and Safari. cli/libexec/mksimdevice.py seeds `Session Language = en-US` in
  com.apple.assistant.backedup, as a Mac's device has it.
- The Maps widget looked up `com.apple.navigationService` ~2000 times a minute. Spotlight polled ManagedConfiguration
  migration.
- cli/libexec/rundev.sh now starts kbd, navd, profiled, assistantd, SafariBookmarksSyncAgent, webbookmarksd and
  Safari.History:
  - kbd: without `com.apple.TextInput` the keyboard client retried for ever and only the first typed character
    arrived.
  - SafariBookmarksSyncAgent: without it Safari reconnected in a loop and crashed in
    `-[WBTabGroupSyncAgentProxy addSyncObserver:]`.
- appprotectiond was tried and dropped: it spins looking up com.apple.dmd, which the runtime doesn't have.

### Tools
- simshim: `name+` in x86code-stderr-progs also sets OS_ACTIVITY_MODE=debug. This had no visible effect.
  Subsystem Logging plists in the runtime aren't honoured either.
- host/simshim/diag/extdiag.m (X86CODE_INSERT=host/simshim/diag/libextdiag.dylib): logs BrowserEngineKit's process launches and
  their completions.
- Probes: tests/probes/taskprobe.swift, wkexprobe.m (build both slices and run on the Mac with simctl spawn to
  compare), entblobprobe.c.
- tools/debug/gcoresim.sh, allstacks.sh, stuck.sh, lastcrash.sh and sigctx.py write cores and maps to
  $X86SIM_SCRATCH (not /tmp). Use sigctx.py without a word count (the default 4000 finds the frame).
- Ghidra headless (~/.local/opt/ghidra_12.1.3_PUBLIC/support/analyzeHeadless) was used with small scripts that
  decompile the callers of a string or symbol. Scripts and projects are in $X86SIM_SCRATCH/gh.

### App compatibility (boot of 2026-10-28 ~06:00, standard cli/libexec/rundev.sh, Metal on)
| App | Opens | Shows |
|---|---|---|
| Settings | yes | list. Row icons gray on that first launch (53); colour after a scroll or on a second launch |
| Calendar | yes | day view with the now-line (46). SIGKILLed ~70 s later (not investigated) |
| Maps | yes | globe with tiles, Places sheet. "No Internet Connection" banner still shown (45) |
| Wallet | yes | Passes and Tickets (47) |
| Messages | yes | two sample conversations (55) |
| Contacts | yes | "No Contacts" (48) |
| Files | yes | Recents, empty (49). Its DocumentManagerUICore.Service helper gets SIGKILLed |
| Fitness | yes | Summary tab, no data (51) |
| Shortcuts, Passwords (Utilities folder) | yes | empty list / welcome sheet |
| Reminders | yes | welcome screen |
| Photos | yes | empty library, then the "What's New" sheet |
| Safari | partly | start page and address bar work, WebKit processes launch; a page doesn't render (S2C hang above), and without DT logging Safari sometimes crashes early |
| News | no | white screen with a spinner |
| Health | no | SIGILL ~3 s after launch |
| Preview | no | blank, then SIGKILL |

The home screen's Maps widget now shows a map (44), and page 2 is Fitness, Contacts, Files, Preview and Utilities (52).
The status-bar clock stops updating after a while (seen in several screenshots, not investigated).

### Open (next)
- Safari: port the rest of willi's S2C stack (darlingserver#10/#13/#14) for the duplicate S2C reply on Safari's main
  thread. Then check the page loads (example.com, then a real site).
- Maps' "No Internet Connection" banner and the -1009 downloads (task 2 of this spike) were not reached. The socket
  options (TCP_NOTSENT_LOWAT, OUT_IF, SIOCGCONNINFO, SO_CONNECTION_IDLE) are still unimplemented.
- Settings icons: on a first launch some or all rows stay gray until a scroll. The cooperative pool fix made most
  of them arrive. What remains is a redraw that doesn't happen without scrolling, plus the icon store losing entries
  across boots (not re-measured).
- Health SIGILL, News spinner, Preview SIGKILL, Calendar SIGKILL after ~70 s.
- Safari without DT logging: early SIGSEGVs in vImage / CABackingStore and a use-after-free in WebBookmarks' sync
  proxy (the last went away with the sync agent running).

Darling changes in this spike (patches/darling-iossim.patch regenerated, 15065 lines):
- libpthread: cooperative threads.
- libsystem_kernel:
  - workqueue WQ_FLAG_THREAD_COOPERATIVE
  - ulock_wait2 (544), plus the ULF_NO_ERRNO ulock stubs (VibeDarling/darling-xnu#15)
  - RPC hooks keep the Linux signal mask (willi/darling-xnu#26)
  - getattrlist parent IDs and mount status
  - shared fsgetpath table
- libkqueue: timer EV_ADD re-arm, errno kept across close.
- darlingserver: duplicate S2C replies and unparseable messages are contained.

Installed without sudo (copy + rename): libsystem_kernel, libsystem_pthread, libsystem_c, libkqueue_sim
(+set_macho_platform), darlingserver.

Other changes:
- simshim: OS_ACTIVITY_MODE=disable by default; `name+` sets debug mode.
- cli/libexec/rundev.sh: kbd, navd, profiled, assistantd and Safari's three daemons.
- cli/libexec/mksimdevice.py: Siri session language.
- Scratch output to $X86SIM_SCRATCH: tools/debug/gcoresim.sh, allstacks.sh, stuck.sh, lastcrash.sh, sigctx.py.

New files: host/simshim/diag/extdiag.m, tests/probes/{taskprobe.swift, wkexprobe.m, entblobprobe.c}.
Screens 43-55.

## Spike 3.10 — Safari renders pages, the clocks, Maps online, Health (2026-10-28)

**Results:**
- **Safari renders web pages.** https://example.com (57) and https://www.wikipedia.org (58: the portal, with live
  search suggestions from its JavaScript) load and draw. The first page that rendered was a Google search (56).
- **The status-bar clock keeps time.** It used to stop at the minute SpringBoard started (mach trap 95 was missing).
- **CLOCK_MONOTONIC is right.** It jumped 1 s back and forth inside every second.
- **Maps no longer shows "No Internet Connection"** (61).
- **Health opens** (60: Summary). **Calendar stayed up** for 100 s and more (59). In 3.9 it was SIGKILLed after ~70 s.
- Typing a URL into Safari now reaches the field in full. Before, only part of it arrived (see the clocks below).

Prior work checked first, as asked:
- willi/darlingserver #10, #13 and #14 are ported, with the parts of #6 they need (semaphore retain/release).
- The VibeDarling org and maclator have nothing on mk_timer_arm_leeway, kern.boottime, SIOCGIFFUNCTIONALTYPE or the
  socket options.

### darlingserver: duplicate S2C replies (willi #10/#13/#14 ported, and the cause fixed)
- **Cause.** A thread could be on darlingserver's work queue twice.
  - An extra `doWork()` resumed a microthread that was waiting for an S2C reply, with no wakeup.
  - Its `_s2cPerform()` then returned "interrupted" after the call had gone out.
  - The client's late reply sat in `_s2cReply` with the semaphore raised, and the next S2C call took it as its own.
  - That next call's real reply was the "duplicate" and was dropped. In the signal path, the client also stayed in
    its S2C interrupt for ever, because the exit semaphore was never raised. That was Safari's hang in
    `mach_make_memory_entry_64`.
- **Fixes:**
  - **#13:** a thread is on the work queue at most once (`claimScheduling()`). An interrupt_enter queued behind a
    pending call is not scheduled twice, and doneWorking reschedules a call that arrived while the thread was running.
  - **x86Code:** when an S2C reply wait ends after the call was sent:
    - a reply that has already arrived is discarded;
    - otherwise the late reply is marked to be dropped when it arrives (`_s2cAbandonedReplies`);
    - deferred replies are sent, and a client in the signal path is released.
  - **#10:** checked-out threads are released, and dead threads leave `task_threads()`. The S2C semaphores are
    retained while in use (#6's retain/release, and KERN_TERMINATED counts as "over"). `_scheduleRelease()` runs once,
    and `~Thread` finds its entry by owner.
  - **#14:** S2C calls skip dead threads (`_performS2C()`, up to 16 tries). Each request is tracked as
    `_s2cRequest`/`_s2cTaken`, so a stale signal is answered at once. `notifyDead()` destroys the enter semaphore.
    mldr's checkin and checkout get the signal path (`madeByMldr`).
  - Adaptations to our tree:
    - `Call` has no `shared_from_this`, so Checkout takes its reference from `activeCall()`.
    - #6's `down_timed` is left out, because #14 removes its only user.
- **Result.** No "already had one pending" line in any of the ~20 boots since. Before, there were several per Safari
  session.
- **mach_memory_entry_ownership / _purgable_control / _access_tracking** were unsafe stubs that aborted darlingserver
  (taking every process down) as soon as WebContent got past the hang. They now validate the entry and answer as XNU
  does with no ledgers:
  - ownership: KERN_SUCCESS;
  - purgable: NONVOLATILE ("never purged");
  - access tracking: KERN_NOT_SUPPORTED.

### The clocks: kern.boottime and mach trap 95
- **CLOCK_MONOTONIC.** tests/probes/sleepprobe showed every sleep taking "1 s longer"; tests/probes/clockprobe showed
  the cause.
  - The sim's libc computes CLOCK_MONOTONIC as realtime minus kern.boottime.
  - Darling reported the boot time with `tv_usec = 0`, so the seconds of CLOCK_MONOTONIC were off by one for part of
    every second.
  - Fix (sysctl_kern.c): the boot time to the microsecond, as CLOCK_REALTIME minus CLOCK_MONOTONIC (the clock
    mach_absolute_time counts), computed once per process. clockprobe: 0 steps in 3 M samples (4 per run before).
- **Status-bar clock.** host/simshim/diag/clkdiag.m traced it.
  - SBUIPreciseClockTimer (display link) ticked, and the lock-screen date provider got its minutes.
  - SBStatusBarStateAggregator's provider sits on SpringBoardFoundation's SBFMachNextMinuteDateProvider. That provider
    arms an mk_timer with `mk_timer_arm_leeway`, which is mach trap 95: "Unimplemented mach trap (95)" in backboardd's
    log too.
  - Fix (mach_traps.c): trap 95 arms the timer as `mk_timer_arm` (continuous time is the same clock here, and the
    leeway only allows coalescing).
  - The status bar now updates every minute, in SpringBoard and in apps.

### Network: interface type, socket options
- **Maps banner.** The decision chain:
  1. Maps' StatusIndicatorManager uses `MKCurrentNetworkConnectionFailureDiagnosis()`.
  2. That reads `-[GEONetworkObserver isNetworkReachable]`.
  3. That is `satisfied && (Wi-Fi || cellular || wired)` from `nw_path_uses_interface_type()`.
  - Our path was satisfied, but its interface type was "other". Network.framework gets it from
    `SIOCGIFFUNCTIONALTYPE` (`_IOWR('i',173)`), which was passed through to Linux and failed ("Passing thru unhandled
    ioctl 0xC02069AD").
  - Fix (sockextra.c): answer from Linux's link type:
    - loopback → LOOPBACK;
    - Ethernet that answers SIOCGIWNAME → WIFI_INFRA;
    - other Ethernet → WIRED;
    - anything else → UNKNOWN.
  - The banner is gone (61).
- **Socket options** (sockextra.c, answered as XNU does):
  - `TCP_NOTSENT_LOWAT` maps to Linux's (25).
  - `SO_CONNECTION_IDLE` (0x1137) is kept per descriptor and cleared on close. Non-INET sockets get EINVAL.
  - `IP_OUT_IF`/`IPV6_OUT_IF` (9696):
    - get: the bound interface (SO_BINDTOIFINDEX), else the one that owns the local address (SIOCGIFCONF, or
      /proc/net/if_inet6 for IPv6), else 0;
    - set: EINVAL.
  - `SO_DEFUNCTOK` (0x1100) is accepted. dnssd_clientstub logged EINVAL for it on every connection.
- **SIOCGCONNINFO is implemented but not enabled.**
  - `sockextra_conninfo()` fills XNU's so_cinforeq: flags, interface, error, addresses, and CIAUX_TCP with
    tcp_connection_info.
  - With it answered, Network.framework took `nw_endpoint_flow_override_connected_on_path()` and aborted on a
    recursive `os_unfair_lock` in `nw_endpoint_flow_copy_path()`. geod and NewsTodayIntents crash-looped. This
    happened with the bound interface and with 0 alike.
  - Bisected to this ioctl. It stays ENOTTY, as before.
- **The remaining -1009s** were not reproduced this time. Safari's bag.itunes.apple.com -1009 happened once and not on
  the strace'd launch, and Maps had none. News still logs `nw_endpoint_flow_failed_with_error ... already failing` for
  one flow (open).

### Apps
- **Health (SIGILL):** `DiskHealthExperienceStore.swift:307` traps when NSFileCoordinator fails, and
  com.apple.FileCoordination (filecoordinationd) wasn't running. It is added to cli/libexec/rundev.sh. Health opens (60).
- **Calendar SIGKILL ~70 s:** not seen again after the clock fixes (100+ s, now-line on time, 59). It was most likely a
  RunningBoard/watchdog deadline compared across the jumping CLOCK_MONOTONIC (not proven).
- **Preview (blank):** its document browser looks up com.apple.FileProvider. With that job added, fileproviderd aborts
  at start. In `_dispatch_bug_kevent_client` ("monitor", "vnode"), an EVFILT_VNODE source is registered on fd -1
  (strace: `fstat(-1)` EBADF from libkqueue). The job is left out of rundev for now (open).
- **News (spinner):** it stays in `FRUserOnboardingOperation ... determining user ID`. Its lookups of
  com.apple.accountsd.accountmanager, com.apple.ak.auth.xpc, locationd and usernotifications fail (the account/CloudKit
  daemons don't run). Its widget extensions NewsToday2 and NewsTodayIntents abort in
  `-[NSFileCoordinator _withAccessArbiter:...]` at boot (open).
- **Safari's early crash without DT logging** was not seen in ~10 Safari launches with `X86CODE_NODT=all` this spike.
- **Safari's WebKit GPU process** once aborted with "Failed to map IOSurface into user address space": `xpc_shmem_map`
  of an iosurfaced surface returned 0 during a Wikipedia reload. Then the page stayed blank until Safari was reopened
  (open).
- **openurl job (host/openurl/openurl.m)**, an attempt at `simctl openurl`:
  - lsd aborted in `-[FBSOpenApplicationService openApplication:withOptions:clientHandle:completion:].cold.1` (a
    parameter assertion) when a launchd job asked it to open a URL.
  - In one boot its usleep hung in darlingserver's `semaphore_timedwait` (not understood; waitprobe's 15 timed waits
    were exact).
  - Use typing (cli/libexec/simtouch.py) for Safari.

### Tools
- Shims (X86CODE_INSERT):
  - host/simshim/diag/clkdiag.m: SpringBoard clock path (display link ticks, minute handlers, date providers).
  - host/simshim/diag/geodiag.m: GEONetworkObserver path updates and Maps' StatusIndicatorManager state; GEODIAG_PROGS,
    default `Maps,geod`.
- Probes:
  - tests/probes/timerprobe.m: NSTimer/dispatch soak.
  - sleepprobe.c: usleep/nanosleep soak.
  - waitprobe.c: semaphore_timedwait, mach_msg timeout, nanosleep.
  - clockprobe.c: clock consistency, run with `cli/libexec/run-sim.sh tests/probes/clockprobe`.
  - openurl.m (see above).
- `cli/libexec/run-sim.sh` runs a probe beside a running boot. Darling libraries installed by copy+rename are only picked
  up after the container restarts (overlay).
- darlingserver logs `schedule` warnings (dropped duplicate work-queue entries) with
  `DSERVER_LOG_CATEGORIES=schedule`. That variable reaches darlingserver through the darling launcher.

### App compatibility (boots of 2026-10-28 ~07:00-08:05, standard cli/libexec/rundev.sh, Metal on)
| App | Opens | Shows |
|---|---|---|
| Safari | yes | example.com (57), wikipedia.org with search suggestions (58), Google (56). The GPU process once failed to map an IOSurface on reload |
| Maps | yes | globe, tiles, Places sheet, **no banner** (61) |
| Health | yes | Summary, "Loading Highlights" (60) |
| Calendar | yes | day view, now-line on time, stays up (59) |
| Settings | yes | list. Row icons need a scroll on first launch (not re-examined) |
| Wallet, Messages, Contacts, Files, Fitness, Shortcuts, Passwords, Reminders, Photos | yes | as in 3.9 (not re-tested) |
| News | no | white screen with a spinner (needs the account/CloudKit daemons) |
| Preview | no | blank (needs fileproviderd, which aborts) |

The status-bar clock now keeps time everywhere.

### Open (next)
- Preview: fileproviderd's EVFILT_VNODE source on fd -1 (find which open fails first).
- News: accountsd/akd/cloudd; its widget extensions' NSFileCoordinator abort.
- SIOCGCONNINFO: why answering it sends Network.framework into the recursive lock.
- WebKit GPU process: the IOSurface `xpc_shmem_map` failure.
- Settings first-launch icons (not reached this spike); lsd's openURL from a launchd job.
- The single hung usleep in openurl.

Darling changes in this spike (patches/darling-iossim.patch regenerated, 16506 lines):
- darlingserver:
  - willi #10, #13 and #14, plus #6's semaphore retain/release;
  - S2C abandoned-reply handling;
  - mach_memory_entry_ownership, _purgable_control and _access_tracking.
- libsystem_kernel:
  - mach trap 95 (mk_timer_arm_leeway);
  - kern.boottime to the microsecond;
  - sockextra.c: SIOCGIFFUNCTIONALTYPE, TCP_NOTSENT_LOWAT, SO_CONNECTION_IDLE, IP(V6)_OUT_IF, SO_DEFUNCTOK, and the
    unused SIOCGCONNINFO.

Installed without sudo (copy + rename): darlingserver, libsystem_kernel.

Other changes:
- cli/libexec/rundev.sh: com.apple.FileCoordination.
- New files:
  - shims: clkdiag.m, geodiag.m;
  - probes: timerprobe.m, sleepprobe.c, waitprobe.c, clockprobe.c, openurl.m.
- Screens 56-61.

## Spike 3.11 — Preview, News, openurl, file watches, iopolicy (2026-10-28)

**Results:**
- **Preview opens** (62): its document browser with New Document / Scan Documents. fileproviderd no longer crashes
  at start. Recents shows "Content Unavailable" (Spotlight, see Open).
- **News shows its signed-out UI**: the welcome screen (63), then Today with live stories and photos (64). The
  account, CloudKit, push and identity daemons now run, and the widget extensions no longer abort.
- **`cli/libexec/simtouch.py openurl <url>`** works like `simctl openurl` (65). **`cli/libexec/simtouch.py app <bundle id>`**
  opens any app, including ones the home screen doesn't show.
- **Dispatch vnode sources work** (EVFILT_VNODE in libkqueue). Before, every watch on a file outside the Darling
  prefix failed, which is every file in the device's data directory.
- **Safari:** 12 alternating loads and reloads of wikipedia.org and en.wikipedia.org/wiki/Linux all rendered (66).
  The GPU process's IOSurface map failure was not reproduced. A likely cause is fixed (see below), but it is not
  proven.
- **Settings row icons** show on a first launch even with the icon cache deleted (67). No scroll is needed.
- **App survey** (68, 69): 15 apps open; see the table.

Prior work checked first, as asked: research/willi, VibeDarling and linuxkid473 have nothing on
SIMULATOR_MEMORY_WARNINGS, libkqueue vnode paths, iopolicysys or lsd's openURL client handle. willi's darling-xnu#3
only adds `posix_spawnattr_setdataless_iopolicy_np`.

### fileproviderd: the simulator's memory-pressure source is a file watch (SIMULATOR_MEMORY_WARNINGS)
- **Finding the caller.** host/simshim/diag/vnodediag.c logs:
  - every `dispatch_source_create(VNODE)`;
  - every EVFILT_VNODE `EV_ADD` that reaches `kevent_qos`/`kevent_id` with a bad ident, with a backtrace. The knote
    is registered synchronously from `dispatch_resume`, so the backtrace shows the creator.
- **Cause.** The bad watch came from LaunchServices' `_LSSessionInitMemoryWarningListener`, which calls
  `dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, ...)`.
  - The simulator's libdispatch implements memory-pressure sources as an EVFILT_VNODE (NOTE_ATTRIB) watch on the file
    named by `$SIMULATOR_MEMORY_WARNINGS` (`_dispatch_ios_simulator_memorypressure_init`).
  - Without that variable the fd is -1, and libdispatch crashes the process ("monitor", "vnode").
  - CoreSimulator sets the variable for every job. A Mac's device has it, plus the empty file
    data/var/run/memory_warning_simulation.
- **Fix:** cli/libexec/mksimdevice.py sets the variable and creates the file.

### libkqueue: vnode watches on files outside the prefix (Darling)
- With the variable set, the watch still failed:
  - libkqueue's `add_watch()` turns the fd into a path with readlink(/proc/<pid>/fd/<n>), which gives a *Linux*
    path (/home/...).
  - Darling's `inotify_add_watch()` then expands every path into the prefix (~/.darling/home/...), so it
    returned ENOENT.
  - That is every file in the device data directory. Files inside the prefix worked only by accident.
- **Fix** (src/linux/vnode.c, `#ifdef DARLING`): watch the open file itself with a raw Linux `inotify_add_watch` on
  /proc/self/fd/<n>.
- **Turning the watches on exposed upstream bugs, also fixed:**
  - **IN_CLOSE.** The mask included IN_CLOSE, so the knote was deleted whenever *any* process closed the file
    (cfprefsd reading a plist). Darling's close hook already drops a closing fd's knotes.
  - **Abort on an empty read.** A copyout with nothing to read returned -1, and `linux_kevent_copyout()` called
    abort(). runningboardd, backboardd and SpringBoard aborted at boot. Now the inotify fd is non-blocking and an
    empty read drops the event.
  - **Wrong fflags.** Events reported every requested fflag, not the ones that happened. An event that matches none
    is dropped.

### iopolicysys (322) kept nothing (News widgets: NSFileCoordinator assertion)
- NewsToday2 and NewsTodayIntents aborted with
  `Assertion failed: (materializationOffState == IOPOL_MATERIALIZE_DATALESS_FILES_OFF), function -[NSFileCoordinator(NSPrivate) _withAccessArbiter:invokeAccessor:orDont:andRelinquishAccessClaim:]`.
- **Cause.** NSFileCoordinator sets the thread's IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES policy to OFF and reads it
  back. Darling's `sys_iopolicysys` returned 0 without filling anything in.
- **Fix** (misc/iopolicysys.c): GET returns what SET stored.
  - Storage: 4 bits per type, for the process, the thread (a Darling TSD slot, `__PTK_DARLING_IOPOL` = KEY5) and
    DARWIN_BG.
  - Commands, scopes and policy values are validated as XNU does.
  - Nothing acts on a policy.

### News: the account daemons
- **Before:** News waited "determining user ID", because the lookups of com.apple.accountsd.accountmanager and
  com.apple.ak.auth.xpc failed.
- **Fix.** cli/libexec/rundev.sh now starts these daemons. A haiku agent listed the jobs running in the Mac's iOS 26.5
  simulator and matched them to their plists:
  - accountsd, akd, cloudd, apsd, appleaccountd, usernotificationsd, locationd, followupd, newsd,
    identityservicesd, ids_simd, cdpd.
- **Result.** News opens to its welcome screen, then Today. Apps now show the notification and location permission
  prompts (68, 69).
- **amsaccountsd is left out.** It traps in CloudKit (`CKSymptomDiagnosticsReporter`, via
  `CKContainerImplementation`) right at boot.
- apsd polls interface ioctls: SIOCGIF* is passed through about 150 times each in 3 minutes (harmless).

### openurl: through FrontBoard, not lsd
- **Before:** lsd aborted in `-[FBSOpenApplicationService openApplication:withOptions:clientHandle:completion:]`.
  - In lsd, the method requires `clientHandle` to be non-nil and of the expected class.
  - lsd passes the caller's process handle there, and has none for a launchd job.
- **New route.** host/openurl/openurl.m does what lsd does once it has picked the app:
  - It picks the app with `applicationsAvailableForHandlingURLScheme:` (http(s): Safari).
  - It calls `FBSOpenApplicationService serviceWithDefaultShellEndpoint`, `openApplication:withOptions:completion:`
    with `__PayloadURL`.
  - SpringBoard checks com.apple.frontboard.launchapplications. The binary carries it in `__TEXT,__entitlements`
    (openurl.entitlements), which the simulator reads in place of a signature.
- **Where it reads URLs.** The job reads them from data/tmp/x86code-openurl and logs to
  data/var/log/x86code-openurl.log.
  - The old path, the prefix's /private/var/tmp, didn't work: a file written there from Linux goes into the
    overlay's upper directory, and the container didn't see it.
- cli/libexec/rundev.sh starts the job (com.x86code.openurl). `simtouch.py openurl <url>` and `simtouch.py app <bundle id>`
  write to that file.
  - A line `ls <url>` still goes through lsd, to reproduce the abort.
- The lsd abort itself is not fixed (open).

### darlingserver: interrupted S2C mmap (IOSurface map failure, likely cause)
- **How it fails.** `xpc_shmem_map()` → `mach_vm_map()` of a memory entry → `Thread::mapFile()` → an S2C mmap in
  the client.
  - When the S2C wait is interrupted (a signal, or the wait ends early after Spike 3.10's abandoned-reply
    handling), `_mmap()` returns EINTR and the map fails with "S2C mmap call failed".
  - XNU's vm_map doesn't fail that way.
- **Fix.** `mapFile()` and `allocatePages()` retry up to 8 times on EINTR, and log "S2C mmap interrupted, retrying".
- **Not proven.** The original failure was not reproduced (12 navigations above), so this fix is not confirmed.

### SpringBoard swipes (seen once)
- In one boot, SpringBoard ignored every swipe: no paging and no Spotlight pull-down. Taps worked, and an app's
  scroll views scrolled. The next boot was fine. Not investigated.

### Tools
- host/simshim/diag/vnodediag.c (X86CODE_INSERT=host/simshim/diag/libvnodediag.dylib): logs vnode sources and bad-ident
  EVFILT_VNODE registrations with backtraces, to $X86SIM_SCRATCH/vnodediag.<pid>.log.
- **Pitfall: replace a dylib or binary by rename (`.new` + mv), never by writing over it.** Overwriting
  libvnodediag.dylib in place while it was mapped crashed launchd_sim and took the whole device down.
- host/openurl/openurl.m + openurl.entitlements. Build on the Mac with
  `-Wl,-sectcreate,__TEXT,__entitlements,openurl.entitlements`.
- cli/libexec/simtouch.py: `openurl <url>` and `app <bundle id>`.
- To see a launchd job's os_log, add its process name (e.g. `fileproviderd`) to x86code-stderr-progs. The output
  goes to ~/.darling/private/var/tmp/x86code-<name>.log.

### App compatibility (boots of 2026-10-28 ~08:40-09:15, standard cli/libexec/rundev.sh, Metal on)
Not in the runtime: App Store, Camera, Clock, Notes, Music, Podcasts, TV, Translate, Freeform, Journal, Books,
Calculator, Mail, FaceTime, Weather. Their .app directories hold only localizations, as in the iOS simulator on a Mac.
| App | Opens | Shows |
|---|---|---|
| Safari | yes | example.com via openurl (65), en.wikipedia.org/wiki/Linux, wikipedia.org; 12/12 loads and reloads rendered (66) |
| News | yes | welcome (63), Today with live stories (64), notification prompt |
| Preview | yes | document browser (62). Recents: "Content Unavailable" (Spotlight); Browse: empty |
| Files | yes | "Content Unavailable" in Recents (Spotlight query -2001, searchd crashes) |
| Settings | yes | all row icons on first launch, also with the icon cache deleted (67). The Game Center icon is a gray wireframe |
| Maps | yes | globe and Places sheet (69) |
| Health | yes | Summary, notification prompt (69) |
| Calendar | yes | day view, now-line (69) |
| Fitness | yes | Summary, notification prompt (68) |
| Wallet | yes | Passes, location prompt (68) |
| Messages | yes | two conversations (68) |
| Contacts | yes | "No Contacts" (68) |
| Passwords | yes | welcome (68) |
| Reminders | yes | welcome (68) |
| Photos | yes | "What's New" (68) |
| Shortcuts | yes | All Shortcuts (69) |

### Open (next)
- **Files/Preview "Content Unavailable":** Recents is a CoreSpotlight query, and it fails with
  CSSearchQueryErrorDomain -2001.
  - With com.apple.searchd added, searchd SIGSEGVs in `SIOpenIndexAtPathWithCallbacks` (SpotlightIndex `makeThreadId`).
  - syncdefaultsd (com.apple.kvsd) SIGILLs.
  - Neither is in rundev.
- **fileproviderd SIGABRT, seen once:** `-[FPDDomain volume]` was nil, an assertion at FPDDomain.m:218 while a
  domain indexer started.
- **lsd's openURL from a launchd job** (no client RBSProcessHandle). It is worked around by openurl.
- amsaccountsd trap in CloudKit; SIOCGCONNINFO (3.10); SpringBoard ignoring swipes (once); the Game Center icon.
- The IOSurface map failure (unconfirmed fix).

Darling changes in this spike (patches/darling-iossim.patch regenerated, 16737 lines):
- libkqueue (src/linux/vnode.c): vnode watches through /proc/self/fd, no IN_CLOSE, non-blocking inotify, empty
  reads dropped, fflags as they happened.
- libsystem_kernel: iopolicysys keeps its policies (tsd.h: `__PTK_DARLING_IOPOL`).
- darlingserver: S2C mmap retried on EINTR.

Installed without sudo (copy + rename, scratch inst.sh): libkqueue_sim (+set_macho_platform), libsystem_kernel,
darlingserver.

Other changes:
- cli/libexec/mksimdevice.py: SIMULATOR_MEMORY_WARNINGS and data/var/run/memory_warning_simulation.
- cli/libexec/rundev.sh adds these jobs:
  - com.apple.FileProvider;
  - accountsd, akd, cloudd, apsd, appleaccountd, usernotificationsd, locationd, followupd, newsd,
    identityservicesd, ids_simd, cdpd;
  - com.x86code.openurl.
- cli/libexec/simtouch.py: `openurl`, `app`.
- New files: host/simshim/diag/vnodediag.c, host/openurl/openurl.entitlements. openurl.m was rewritten.
- Screens 62-69.

## Spike 3.12 — Spotlight, openURL through lsd, file watches, permission prompts, robustness (2026-10-28)

**Results:**
- **Spotlight search works** from the home screen (pull down): "Maps" gives the Top Hit (Maps opens from it),
  suggestions, App Store, websites and Siri Knowledge (71). searchd builds its index; Siri Suggestions show icons.
- **Files and Preview Recents** say "No Recents" (70, 75) instead of "Content Unavailable": the CoreSpotlight query
  now succeeds. Files' Browse shows **On My iPhone** with its files (74), Preview's browser too (73).
- **openURL from a launchd job goes through lsd** (72: Safari on example.com). lsd no longer aborts.
- **No permission prompt covers the app survey** any more (76; 77 is before): the answers are pre-seeded and what is
  left is tapped away.
- **Crashes:** 3 boots × 16 apps before and after; see "Robustness". Real crashes went from 7 (and 943 of 1024 inotify
  instances) to 2 in the first after-boots and 0 in the last two. On the way, darlingserver (whole device) and
  backboardd crashes were found and fixed.

Prior work checked first, as asked:
- **Ported:** willi/darlingserver#11 (EXCEPTION_IDENTITY_PROTECTED, which searchd needed; its research notes even name
  searchd) and willi/darling-xnu#6's per-address ulock waiter table.
- **Nothing there:** VibeDarling and maclator have nothing on guarded_pwrite_np, f_fstypename, XPC_SERVICE_NAME or
  libkqueue's vnode enable/modify.

### searchd: the exception port, then every index write (Darling)
- **The exception port.** searchd crashed in `makeThreadId` (SpotlightIndex) within a second of every start, on
  `ContentIndexExceptionHandler.c:1174 krc==KERN_SUCCESS`.
  - Each index thread calls `thread_set_exception_ports(self, EXC_MASK_BAD_ACCESS|EXC_MASK_SOFTWARE, port,
    MACH_EXCEPTION_CODES|EXCEPTION_IDENTITY_PROTECTED, x86_EXCEPTION_STATE64)`. The duct-tape XNU (7195) answered
    KERN_INVALID_ARGUMENT for behavior 4.
  - **Fix:** willi/darlingserver#11, ported as is: the set/swap calls accept the behavior, and `exception_deliver()`
    sends `mach_exception_raise_identity_protected` (2408). The stale MIG outputs (mach_exc*) were deleted, so the
    build regenerated them (willi's mig-deps isn't ported).
- **Every index write.** searchd then stayed up, but every open said "Can't open dataclass:Priority - Not fully
  created".
  - The index's errorFile1 held `ContentIndexCommon.c:297: indexState fd_create fail`, and strace showed
    "Unimplemented syscall (486)".
  - SpotlightIndex writes each index file with **guarded_pwrite_np**. Darling had no syscalls 484-487.
  - **Fix** (guarded/guarded_io_np.c): guarded_open_dprotected_np, guarded_write_np, guarded_pwrite_np and
    guarded_writev_np are the unguarded calls (Darling keeps no guards).
  - Also, guarded_open_np and guarded_close_np wrote 0 into the caller's guard, which is an input (`const
    guardid_t*`). They don't any more.
  - The index is now built as on a Mac (0.indexHead, dbStr-*.map, .store.db...).
  - The device's stale half-made index (from the crashes) had to be deleted once:
    data/Library/Spotlight/CoreSpotlight/{Priority,index.spotlightV2}.

### Spotlight: CoreSpotlight's strings, by absolute path
- **Symptom.** Spotlight crashed on the first typed query with `-[__NSPlaceholderDictionary
  initWithObjects:forKeys:count:]: attempt to insert nil object` in `-[_CSSuggestionToken
  initWithUserString:displayString:...]`. The unwinder then SIGSEGVed in libdispatch's callout personality, and the
  crash reporter saw that instead.
- **Cause.** The nil was `CSGetLocalizedString(@"Photo")`. CoreSpotlight loads its strings with
  `[NSBundle bundleWithPath:@"/System/Library/Frameworks/CoreSpotlight.framework"]`, which is a host path. On a Mac
  that resolves (macOS has the framework). Darling's prefix has no such framework.
- **Fix** (cli/libexec/run-launchd-sim.sh): link the runtime's CoreSpotlight.framework into the prefix's
  /System/Library/Frameworks where Darling has none. The list can grow if other frameworks turn out to do the same.

### darlingserver: task_deallocate(TASK_NULL) (Darling)
- **Symptom.** In the first boot where Spotlight showed web results, darlingserver SIGSEGVed in
  `_Xmach_memory_entry_ownership` → `task_deallocate` → `os_ref_release`. Every process went down with it.
- **Cause.** The MIG stub releases what `convert_port_to_task()` gave it, and that is TASK_NULL for a null owner port
  (WebKit giving up ownership of shared memory). XNU's task_deallocate returns for TASK_NULL; the duct-tape one
  dereferenced it.
- **Fix:** task_deallocate and thread_deallocate return on NULL.

### lsd's openURL from a launchd job: XPC_SERVICE_NAME
- **The assertion.** lsd's log had the real message: `failure in openApplication:withOptions:clientHandle:completion:
  (FBSOpenApplicationService.m:91) : what part about LaunchServices-only did you not understand?`.
  - The check is `[[NSProcessInfo processInfo] bs_jobLabel] isEqualToString:@"com.apple.lsd"`, and bs_jobLabel is
    `$XPC_SERVICE_NAME`.
  - Every job had XPC_SERVICE_NAME=com.apple.CoreSimulator.SimDevice.<udid>. cli/libexec/mksimdevice.py put that into
    launchd_sim's environment, and launchd_sim sets a job's XPC_SERVICE_NAME to its label only when the variable
    isn't already there. On a Mac, lsd has XPC_SERVICE_NAME=com.apple.lsd.
  - So lsd aborted on every open that came from a process, not only from launchd jobs. Apps' URLs reached FrontBoard
    by other routes.
- **Fix:** sim.env no longer sets XPC_SERVICE_NAME. The `ls <url>` line of the openurl job (LSApplicationWorkspace
  openURL:) opens Safari (72). `simtouch.py openurl` keeps its FrontBoard route.
- **Also changed** (Darling, not the cause): proc_name(), proc_pidinfo's pbi_comm/pbi_name and KERN_PROC's p_comm
  are the executable's name, not "mldr". It is the last path component of argv[0] for mldr processes.
  BSProcessHandle descriptions said "mldr:<pid>".

### libkqueue: every re-enabled vnode watch leaked an inotify instance (Darling)
- **Symptom.** fileproviderd crashed in every before-boot. `_dispatch_bug_kevent_client("vnode", "add")` with
  EMFILE, made fatal by its LIBDISPATCH_STRICT=YES.
- **Cause.** A sampler (inotify fds per process, every 5 s) showed the simulator's inotify instances climbing to 943
  during an app survey. The user limit is 1024 (fs.inotify.max_user_instances), and the desktop uses the rest.
  libkqueue keeps one inotify instance per vnode knote:
  - EV_ENABLE on a knote that is already watching ran `add_watch()` again. That made a second instance and lost the
    first, which stayed open and in the epoll set.
  - EV_ADD on an existing knote went to `knote_modify`, a stub that failed.
- **Fix** (src/linux/vnode.c): enable on a watching knote is a no-op. modify takes the new fflags and re-adds the
  watch. A new knote starts with no inotify fd.
- **Result.** The peak in the after-boots is 487, with Spotlight's daemons added.

### fileproviderd: no volume for the data, so no providers (Darling)
- **Symptom.** All providers (On My iPhone, Photos, iCloud Drive) were "disabled". Files' Browse was empty, and a
  domain without a volume asserted (`FPDDomain must have a volume attached`, the 3.11 SIGABRT).
- **Cause.** fileproviderd never got past its volume list. `-[FPDVolume initWithStatFS:...]` requires
  `FPVolumeFPFSSupported()`, and that takes only a local, writable, unquarantined volume whose f_fstypename is
  **"apfs"**. Ours said "btrfs" ("Non-eligible device ...: non-apfs", INFO level, not visible in our logs).
- **Fix** (stat/statfs.c, getfsstat.c): Linux's local block file systems (btrfs, ext*, xfs, f2fs, zfs, bcachefs, jfs,
  reiserfs) and the overlay of Darling's prefix report f_fstypename "apfs". The iOS simulator only runs on APFS.
- **Result.** Domains, Domains.plist and the On My iPhone storage are created as on a Mac. Files lists the files there
  (74). No fileproviderd crash in the last two after-boots.
- **statfs f_mntfromname.** It reports the kernel's device name (/dev/dm-0) for a /dev/ symlink such as
  /dev/mapper/luks-<uuid>. CacheDelete refused names longer than 32 bytes ("bsd_name_len (32) too short").

### backboardd: pthread_dependency_wait_np and the ulock waiter count (Darling, willi/darling-xnu#6 ported)
- **Symptom.** backboardd SIGILLed once at boot in `pthread_dependency_wait_np` (CoreMotion/CoreLocation):
  `__ulock_wait() failed`, -ret = -1.
- **Cause.** Our ulock_wait counted waiters in 512 hashed buckets and returned the count. A waiter on a colliding
  address made a single waiter see 1 "remaining", and pthread_dependency crashes on anything but 0.
- **Fix:** willi/darling-xnu#6's exact per-address table (sigsafe lock, reset in the fork child).

### Permission prompts: pre-seeded, and answered by the test tooling
- **cli/libexec/seedperms.py** (rundev runs it before every boot; X86CODE_SEEDPERMS=0 to skip) writes the answers a test
  device would have, for the 16 surveyed apps:
  - **TCC.db:** kTCCServiceAll and the usual services, allowed, as `simctl privacy grant all` writes them.
  - **locationd clients.plist:** Authorization 2, as `simctl privacy grant location` leaves it.
  - **BulletinBoard VersionedSectionInfo.plist:** an authorized section per app, cloned from SpringBoard's Files
    section (fallback: cli/libexec/seedperms-section.plist).
  - Existing answers are kept.
- **What it does on screen.** The notification prompts (Health, Fitness) and Maps' location prompt are gone from the
  second seeded boot on.
  - Maps then asks "Allow widgets from Maps to use your location?" for its widget, which isn't seeded.
  - That prompt, and anything else, is handled by **`cli/libexec/simtouch.py dismiss`**. It OCRs the frame (tesseract with
    tessdata_fast's eng.traineddata in $X86SIM_SCRATCH/tessdata, because the system has no language
    data) and taps "Allow While Using App", else the right-hand "Allow". It never taps "Don't Allow": a "Don't Allow"
    with no "Allow" found is mirrored to the right-hand button.
- **cli/libexec/apptest.sh <out dir> [ids]** is the app pass: open, dismiss, wait, dismiss, screenshot, home, then the
  crash summary (exits.txt, cores.txt).

### Daemons added to the standard boot (cli/libexec/rundev.sh)
- **Spotlight:** searchd and corespotlightservice.
- **What a Mac's simulator runs that Spotlight, Files and others look up:** dasd, contactsd, assistant_cdmd,
  mobileassetd, biomed, parsecd (web results), pasted (the pasteboard), bird, swcd, intelligenceplatformd,
  textunderstandingd.
- **Left out:**
  - syncdefaultsd (com.apple.kvsd) and amsaccountsd trap in CloudKit (see Open);
  - suggestd SIGSEGVs at boot.
- The daemons a Mac's device runs and ours doesn't were listed with `simctl spawn launchctl list`.

### Robustness (3 boots each, 16 apps, then home; "after" = with this spike's fixes)
| | before | after |
|---|---|---|
| fileproviderd (vnode add EMFILE / no volume) | 3 SIGILL | 2 SIGABRT, then 0 (from the apfs fix on) |
| SpringBoard SIGSEGV (MTLSimDevice commandBufferForReference) | 1 | 0 |
| WebKit Networking/GPU SIGKILL | 1 | 0 |
| News SIGKILL (killed after going home) | 0 | 2 |
| widget extension SIGKILLs (chronod, per boot) | 4 | 3-6 |
| inotify instances, peak (limit 1024) | 943 | 487 |
| found and fixed on the way | backboardd SIGILL (ulock count); darlingserver SIGSEGV (all processes); searchd SIGSEGV (every start); Spotlight SIGSEGV (every search); lsd SIGTRAP (every openURL) | |

Per-boot results are in $X86SIM_SCRATCH/robust/{before,after,check}-N/ (exits.txt, cores.txt,
screenshots).

### App compatibility (boots of 2026-10-28 ~11:05-11:45, standard cli/libexec/rundev.sh, Metal on)
| App | Opens | Shows |
|---|---|---|
| Spotlight | yes | home-screen search: Top Hit, suggestions, App Store, websites (71) |
| Files | yes | Recents: "No Recents" (70). Browse: On My iPhone with its files (74). Opening a document crashes Files (see Open). Search finds nothing |
| Preview | yes | document browser; Recents: "No Recents" (75); On My iPhone (73) |
| Safari | yes | example.com, also opened by lsd (72) |
| Maps | yes | globe and the Apple Ads sheet. The location prompt is answered (seeded) |
| Health, Fitness | yes | Summary, no notification prompt |
| News | yes, slow | Today with stories in two of four boots, still black after 20 s in the others. Killed twice after going home |
| Settings | slow | black after 20 s in two of four boots, list in the others |
| Calendar, Wallet, Messages, Contacts, Passwords, Reminders, Photos, Shortcuts | yes | as in 3.11, no prompts (76) |

### Open (next)
- **Files can't open a document.** DocumentManagerUICore.Service is SIGKILLed, then Files SIGSEGVs in a navigation
  transition (`-[_UIViewControllerTransitionContext _runAlongsideCompletions]`).
  - So nothing becomes "recent", and Recents stays empty with no error.
  - fileproviderd logs "can't get item for URL <GroupContainers>/.../File Provider Storage ... Cocoa 256" for the
    LocalStorage extension, and FP items aren't in the Spotlight index (Files search: no results).
- **CloudKit accountInfo:** cloudd answers `accountInfoWithCompletionHandler:` with (nil, nil).
  - The CKContainer client then traps (`$sSo28CKSymptomDiagnosticsReporterCMa`) in syncdefaultsd and amsaccountsd.
  - A Mac's cloudd logs the same "Could not create primary backing account" but answers. Not found yet; debug-level
    os_log doesn't reach our DT-mode logs, which made it hard to follow.
- **SpringBoard SIGSEGV in MTLSimDevice commandBufferForReference:** a command buffer completion (message 101) for a
  ref the client no longer has. Seen once in 3 before-boots.
- **locationd's widget authorization** isn't seeded (dismissed by OCR instead). News and Settings are slow to draw.
- The SpringBoard-ignores-swipes boot (3.11) was not seen in ~15 boots. A slow pull-down (600 ms) opens Spotlight; a
  300 ms one often doesn't (the same on a Mac?).

Darling changes in this spike (patches/darling-iossim.patch regenerated, 17445 lines):
- darlingserver:
  - willi #11 (EXCEPTION_IDENTITY_PROTECTED);
  - task_deallocate/thread_deallocate of NULL.
- libsystem_kernel:
  - guarded_open_dprotected_np/write/pwrite/writev_np (484-487), and the guard no longer overwritten;
  - f_fstypename "apfs" for local disk file systems, f_mntfromname /dev/dm-N;
  - process names (proc_name, pbi_comm/pbi_name, p_comm);
  - per-address ulock waiters (willi xnu#6).
- libkqueue: vnode knote enable/modify (inotify leak).

Installed without sudo (copy + rename, scratch boot.sh): darlingserver, libsystem_kernel, libkqueue_sim.

Other changes:
- cli/libexec/mksimdevice.py: no XPC_SERVICE_NAME in launchd_sim's environment.
- cli/libexec/run-launchd-sim.sh: CoreSpotlight.framework link.
- cli/libexec/rundev.sh: the new daemons, and seedperms.
- host/simshim/compat/simshim.c: a process named in x86code-stderr-progs logs even when launchd gave it
  OS_ACTIVITY_MODE=disable (every launchd job), so daemons' os_log can be read now.
- New files:
  - cli/libexec/seedperms.py and seedperms-section.plist, cli/libexec/apptest.sh;
  - cli/libexec/simtouch.py `dismiss`;
  - tests/probes/fpprobe.m (LaunchServices' File Provider extension point; matches the Mac's 4 extensions).
- Screens 70-77.

## Spike 3.13 — Smooth frames, CloudKit, Files opens documents, time zone (2026-10-28)

**Results:**
- **Frames.** The device now presents up to 85–100 frames/s during scrolling and 93 during home-screen page swipes.
  Before, it was 16 and 26. The main limit was darlingserver: ~75% of its single worker thread went into `std::regex`
  on `/proc/<pid>/maps`. simfb now publishes frames through a shared-memory ring, and simview (GTK 4) and simstream
  read it. The public stream is 60 fps with 0.75 s segments.
- **CloudKit answers** (`accountStatus=No account`, as on a Mac). syncdefaultsd and amsaccountsd no longer trap, and
  both are in the standard boot.
- **Files opens documents.** The text file opens in Quick Look (78) and then shows in Recents (81). A PNG goes to
  Preview, which lists it with the right size (79).
- **News and Settings** drew within 6–10 s of a cold launch (82), and the app survey passed with no crashes (83).
- **Time zone.** The device shows local time (80). It was on UTC before.

Prior work checked first: research/willi, VibeDarling and linuxkid473 have nothing on fsctl/ffsctl, chflags,
PROC_PIDPATHINFO's return value, getattrlist's ATTR_VOL_INFO rule or FileProvider's sim2host service.

### Frame rate: before and after
Measured by cli/libexec/fpsmeter.py, which polls the frame sequence every 2 ms. The scenario is
$X86SIM_SCRATCH/fps/scenario.sh: 6 home-screen page swipes, open Settings, 6 Settings scrolls, go home.
"Peak" is the best 1 s window. "Frames" is what reached Linux in the measured window.

| Scenario | before: peak fps / frames | after: peak fps / frames | after: presents/s, GPU wait, copy |
|---|---|---|---|
| home-screen swipes ×6 (8.5 s) | 26 / 82 | 93 / 316 | 41/s, 10.1 ms, 1.5 ms |
| open Settings (6 s) | 29 / 67 | 43 / 54 (cold launch) | 9/s, 10.9 ms, 1.7 ms |
| Settings scroll ×6 (9 s) | 16 / 91 | 85–100 / 466–562 | 54–62/s, 8–10 ms, 1.2 ms |
| app close (home) | 5 / 5 | 59 / 64 | 16.5/s, 16 ms, 1.5 ms |

- **Steps, home swipes / scroll peak fps.**

  | Step | Home swipes | Scroll |
  |---|---|---|
  | Before | 26 | 16 |
  | Shared-memory ring only | 27 | 17 |
  | + darlingserver maps parser | 45 | 100 |
  | + present FIFO | 78 | 100 |

  The ring alone changed nothing: presents themselves were slow.
- **simfb statistics.** These are kept in the ring header (cli/libexec/fbring.py `stats()`):
  - "presents/s" is CA's SwapSubmit rate;
  - "GPU wait" is submit until the last command buffer completes (metalhostd → mtlvk and back);
  - "copy" is the IOSurface → ring memcpy (12 MB).
- **Cost of the old file copy.** With the file dump the copy took 9–14 ms. In the ring it takes 1.1–1.7 ms.

### darlingserver: /proc/<pid>/maps parsed with std::regex (Darling, the frame-rate limit)
- **Measurement.** During page swipes, `top -H` showed darlingserver's worker thread at 86% of a core. It has one
  worker thread, which runs every Mach call of every process.
- **Profile** (`perf record -g --call-graph dwarf` on darlingserver, perf installed with pacman):
  - ~75% of the samples were in `std::regex` (`_Executor::_M_dfs` and related).
  - The caller was `Process::memoryRegionInfo()`, through `dtape_hook_task_get_memory_region_info` ←
    `dtape_share_region` ← `_mach_make_memory_entry` (every xpc_shmem: MTLSimDriver's command-buffer segments,
    CA's) and `x86code_memfd_for_region`.
  - It matched a regex against every line of the maps file. A simulator process has thousands of lines.
- **Fix** (src/process.cpp). The maps file is read in 256 KB chunks. Each line's address range is parsed by hand,
  and only the matching line is parsed in full.
  - After the fix, darlingserver used ~42% of a core over both threads during the same swipes, and no regex was
    left in the profile.

### simfb: presents dropped while waiting for the GPU
- **Cause.** simfb kept one waiting present. One that came while the previous was still waiting for GPU completion
  replaced it ("superseded"). The GPU latency is ~10 ms, so 60% of a home-screen swipe's presents were dropped
  (209 of 345).
- **Fix.** Waiting presents are a FIFO of 2. CA rotates 3 surfaces, so a surface isn't redrawn until 2 newer presents
  are out. A present is copied as soon as every command buffer before it has completed, oldest first. Only a full
  queue drops its oldest. That is now 55 of 372.
- **The swap callback.** It is still answered at once. SimFramebuffer.h notes that CA doesn't throttle on it, so CA
  presents above 60/s when content changes faster, and the viewers cap.

### Frame output: a shared-memory ring
- **Ring layout.** simfb writes each completed present into a ring of 4 slots in /dev/shm/x86code-simfb.ring. It is
  `/Volumes/SystemRoot/dev/shm/...` inside Darling; `$SIMFB_RING` overrides it.
  - The 4096-byte header holds: magic `SFBR`, version, slot count, slot size, width, height, stride, seq and latest
    slot.
  - Each slot has its own seq, publish time (CLOCK_REALTIME ns) and geometry.
  - Then come the present statistics at offset 192.
- **Publishing.** simfb never writes the latest slot. A slot's seq is zeroed before the copy and set after it, then
  latest and seq are published (release stores).
  - A reader copies the latest slot and checks that its seq didn't change. cli/libexec/fbring.py implements this and
    documents the layout.
  - Each process makes a new file and renames it into place, so a reader of an old ring sees its inode go away.
- **framebuffer.bgra** is still written for old tools, at most every 2 s, from the ring. A 2 s timer writes the last
  frame. Every frame used to be written as a 12 MB file plus a rename.
- **Readers.**
  - **cli/libexec/simview.py** is GTK 4 now. The Tk version needed ~200 ms per redraw of a 590×1278 photo image.
    - Each new frame becomes a `Gdk.MemoryTexture` that the GPU scales to the window. The window can be resized.
    - The ring's seq is checked on every frame-clock tick.
    - The input behaviour is unchanged.
    - On a GPU compositor (headless `kwin_wayland --virtual`) it drew 58–60 fps, ~90% of the frames published. The
      rest were above 60 Hz.
    - The draw rate goes to stdout every 5 s. The user's desktop was locked during this spike, so it couldn't be
      measured there.
  - **cli/libexec/simstream.py:**
    - newest ring frame each tick, at 60 fps (it was 30), constant frame rate;
    - stride-wide rows go into ffmpeg, which crops them;
    - NVENC CBR 3 Mb/s, no B-frames, GOP 0.25 s;
    - HLS with 0.75 s segments. 0.5 s segments made `EXT-X-TARGETDURATION:0`, which is invalid.
  - **cli/libexec/progress_site.py** reads the ring. The page's hls.js now catches up (`maxLiveSyncPlaybackRate` 1.2), and
    the text says "60 fps, ~1-2 s behind".
  - **cli/libexec/fb2png.py** reads the ring when given framebuffer.bgra (or "ring") and uses PIL, so apptest and simtouch
    get the newest frame.

### CloudKit: cloudd took every client for dead (Darling: PROC_PIDPATHINFO's return value)
- **The trap.** syncdefaultsd and amsaccountsd trapped at CloudKit+0x7edeb.
  - The symbolizer named the nearest export, `$sSo28CKSymptomDiagnosticsReporterCMa`. The code is a Swift
    async-bridge thunk: a completion handler called with neither a value nor an error traps (`ud2`).
  - The handler was `accountInfoWithCompletionHandler:`. Before it the client logged "Ignoring failed attempt to get
    container proxy ... : (null)".
- **cloudd's answer.** A diagnostic shim in cloudd (host/simshim/diag/ckdiag.m) wrapped
  `-[CKDXPCConnection getContainerScopedDaemonProxyCreatorForSetupInfo:containerScopedClientProxy:completionHandler:]`.
  - Its answer was (nil, nil) for every container (ids.kt, keychain, CloudTabs, applemediaservices...).
  - The block answers nil when `[[self processScopedClientProxy] processIsAttached]` is NO.
  - `processIsAttached` is `CKPIDIsStillAlive(pid)`. That is `proc_pidinfo(pid, PROC_PIDPATHINFO, 1, buf, 1024) == 0`
    with a non-empty path.
- **Cause.** XNU's PROC_PIDPATHINFO returns 0 on success (`proc_pidpathinfo()` never sets retval, and libproc's
  `proc_pidpath()` takes strlen itself). Darling returned the path length.
- **Fix** (misc/proc_info.c): return 0. Only libproc used the value, and it computes strlen.
- **Result.** cloudd hands out `CKDContainer` proxies, and amsaccountsd logs `Account info is <CKAccountInfo ...
  accountStatus=No account ...>`. Neither daemon crashed in the following boots. cli/libexec/rundev.sh starts both
  (com.apple.syncdefaultsd, com.apple.amsaccountsd).

### Files: opening a document needed the whole File Provider file-system layer (Darling + a host stand-in)
Before, tapping a file did nothing, or crashed Files in 3.12. Files logged "Couldn't get FPItem from node", "error
creating FPProviderDomain ... LocalStorage" and -10814. The On My iPhone extension (LocalStorageFileProvider, now in
x86code-stderr-progs) failed on each item. Each fix exposed the next failure, in this order:

1. **getattrlist volume attributes without ATTR_VOL_INFO** (fileproviderd: "Unable to get file system attributes
   for volume path"):
   - FileProvider's `-[NSURL fp_volumeUUID]` asks for ATTR_VOL_UUID alone. XNU's getattrlist doesn't require
     ATTR_VOL_INFO; only setattrlist does.
   - Darling answered EINVAL, so domains had no volume UUID.
   - Fix: getattrlist_generic.c accepts the request.
2. **fsctl/ffsctl (242/245) didn't exist** ("can't obtain VFSItem ... [78: Function not implemented]"):
   - `fpfs_fget_parent_syncroot()` asks APFS for the enclosing sync root, `ffsctl(fd, _IOR('J', 74, u64))`, and any
     error is fatal.
   - New misc/fsctl.c. Unknown requests get ENOTTY, as XNU's VNOP_IOCTL gives, and are logged once.
   - It answers:
     - the sync-root request: 0, none;
     - `_IOW('J', 2, u64)`: keep directory stats (`fpfs_pkg_enable_dirstat`, called for every tracked item),
       accepted;
     - FSIOC_CAS_BSDFLAGS: below.
3. **O_SYMLINK opens gave O_PATH descriptors** ("[9: Bad file descriptor]"):
   - FileProvider opens every item with O_SYMLINK. Darling turned that into Linux O_PATH, and `fgetxattr`/`read` on
     an O_PATH fd fail with EBADF.
   - On XNU, O_SYMLINK only differs from a plain open for a symlink.
   - Fix (fcntl/openat.c): after the O_PATH open, a regular file or directory is reopened through /proc/self/fd with
     the requested access. O_PATH is kept for symlinks, special files, or if the reopen fails.
4. **chflags/fchflags were ENOTSUP and FSIOC_CAS_BSDFLAGS was missing:**
   - libfssync sets UF_TRACKED on every item. The extension trapped: "Utilities.swift:315: Fatal error: call to
     block failed with -1 but errno is not set".
   - New helper/misc/bsdflags.h keeps BSD flags in the xattr `user.x86code.bsdflags`, hidden from listxattr. It is
     used by:
     - chflags and fchflags;
     - the CAS fsctl (as XNU: the actual flags are returned, the new ones set only if the expected ones match, and it
       succeeds either way);
     - stat/lstat/fstat/fstatat `st_flags` (`stat_set_flags64()`);
     - getattrlist ATTR_CMN_FLAGS.
   - Nothing enforces a flag.
5. **Document id and generation count were 0** ("[35: Resource temporarily unavailable]" while libfssync waited for
   a document id):
   - APFS gives a UF_TRACKED item a document id. Ours is the inode's low 32 bits.
   - ATTR_CMN_GEN_COUNT is non-zero for files and directories. It is derived from the modification time.
6. **The host's FileProvider service** ("can't resolve did=... [78]"):
   - In a simulator, FileProvider sends open-by-id, openat/mkdirat/unlinkat/renameat, xattrs, package lookup and
     dataless files to the Mac, over `com.apple.FileProvider.sim2host`. That is the FPSimulatorSupport protocol, from
     `FPSimulatorSupportInterface()`; there are 16 methods, and their signatures are logged at start.
   - It does so only for a host of macOS 15 or later. The version is `kern.osproductversion`, and Darling answered
     "11.7.4".
   - Fixes:
     - sysctl_kern.c: kern.osproductversion is "26.4" in iOS simulator processes (platform 7), the host an iOS 26.5
       simulator runs on. Other processes keep "11.7.4".
     - host/fpsim2host (new; build/mac-build-fpsim2host.sh; rundev job `com.x86code.fpsim2host`, MachServices
       com.apple.FileProvider.sim2host) serves the protocol inside the simulator.
       - Calls are taken in `-forwardInvocation:` with the protocol's own signatures. The completion block is
         called through `-[NSInvocation invokeUsingIMP:]`.
       - It implements requestProtocolVersion (echoes the client's version, 2), openByID (fsgetpath + open), openAt,
         mkdirAt, unlinkAt (recursive with nftw), renameAt (renameatx_np), setOrClearXattr, and pkgLookup ("not a
         package").
       - The rest answer ENOTSUP and are logged (data/var/log/fpsim2host.log).
     - simshim keeps that connection a simulator one, as for Metal and IOSurface.
7. **getattrlist packed directory attributes for files with FSOPT_PACK_INVAL_ATTRS:**
   - The file attributes were shifted by 4 bytes. FileProvider read a 491 KB file's size as 491520 << 32, shown as
     "2.11 PB" in Preview.
   - XNU packs directory attributes only for directories and file attributes only for the rest, even with that
     option. Fixed.

Result:
- "Notes from Linux.txt" opens in Quick Look, "Hello from Linux" (78), and then shows in Files' Recents (81).
- The PNG is handed to Preview. Preview lists it in Recents with the right size (79), but stays in its browser: it
  logs "Error resolving bookmark ... did=791935814 ... -1005", although fpsim2host resolved that id.
- After that hand-off, Files' On My iPhone showed "Content Unavailable" once (survey). A fresh boot lists it (80).

### Time zone: Darling's prefix had no /etc/localtime
- **Symptom.** The status bar showed UTC: 7:22 PM at 13:22 MDT. File times were off by 6 h.
- **Cause.** A simulator on a Mac reads the host's /etc/localtime. Darling's prefix has none.
- **Fix** (cli/libexec/run-launchd-sim.sh): the container's /private/etc/localtime links to the Linux host's zone in
  /usr/share/zoneinfo. Darling's zoneinfo is Linux's, and the TZif files are the same.

### News and Settings "black after 20 s"
- **Not reproduced after these fixes:**
  - the 16-app survey (83);
  - cold launches with frames at 3, 6 and 10 s (82). Settings is black at 3 s and drawn at 6 s. News shows its
    header at 6 s and stories at 10 s;
  - warm launches, drawn within 3 s.
- **Likely cause:** the darlingserver saturation above. An app launch maps many xpc_shmem regions (LaunchServices'
  7.6 MB database, CA, Metal), each with a regex scan of the maps file, while the same single thread runs every Mach
  message of the device. This was not measured before the fix.

### Tools
- cli/libexec/fbring.py: ring reader (`Ring().latest()`, `.seq()`, `.stats()`; `latest_frame()` falls back to
  framebuffer.bgra). `cli/libexec/fbring.py` prints the ring's state.
- cli/libexec/fpsmeter.py `<seconds> [label]`: delivered fps, gaps, and simfb's present statistics over the window.
- host/simshim/diag/ckdiag.m (cloudd: container proxy answers, CKDProcessScopedClientProxy's methods and ivars).
- host/simshim/diag/attrdiag.c now also takes its process names from data/oslog.filter.
- host/fpsim2host, build/mac-build-fpsim2host.sh.
- perf is installed (`sudo perf record -g --call-graph dwarf -p <pid>`). It is the fastest way to find a
  darlingserver hot spot.

### App compatibility (boot of 2026-10-28 ~13:20, standard cli/libexec/rundev.sh, Metal on)
All 16 surveyed apps open and draw, with no crashes or cores in the pass (83).
| App | Opens | Shows |
|---|---|---|
| Files | yes | On My iPhone (80); opens the text document in Quick Look (78); Recents lists it (81) |
| Preview | yes | Recents lists a PNG opened from Files, with the right size (79); doesn't open it (see Open) |
| News | yes | Today with stories: 6–10 s cold (82), 3 s warm |
| Settings | yes | list: 6 s cold (82), 3 s warm |
| Safari, Maps, Health, Calendar, Fitness, Wallet, Messages, Contacts, Passwords, Reminders, Photos, Shortcuts | yes | as in 3.12 (83) |

### Open (next)
- Preview doesn't open a PNG handed over by Files (bookmark `did=` resolution -1005 in Preview).
  - Afterwards Files' On My iPhone said "Content Unavailable" until reboot.
  - LocalStorageFileProvider has no FSEvents service (`FSEvents_connect() => Unknown service name`, "can't get
    volume UUID for 58", no live updates). On a Mac that comes from the host's fseventsd.
- fpsim2host doesn't implement dirEntryHelper, makeDatalessFileAt, materializeFile, markPurgeable, setSyncRoot,
  unsetDatalessBitAt, register/unregisterMaterializationHandler, or package detection.
- simview at 60 fps was measured on a headless KWin, not on the user's desktop (it was locked).
- From 3.12: the SpringBoard MTLSimDevice command-buffer crash, suggestd (left out), and the Game Center icon.

Darling changes in this spike (patches/darling-iossim.patch regenerated from upstream bases, 18777 lines; the method
above now includes iokitd and dyld):
- darlingserver: `memoryRegionInfo()` parses /proc/<pid>/maps by hand (no std::regex).
- libsystem_kernel:
  - PROC_PIDPATHINFO returns 0;
  - fsctl/ffsctl (242/245): APFS sync root, dir stats, FSIOC_CAS_BSDFLAGS;
  - chflags/fchflags and st_flags/ATTR_CMN_FLAGS (helper/misc/bsdflags.h, `stat_set_flags64`);
  - O_SYMLINK on a regular file or directory gives a real descriptor;
  - getattrlist:
    - volume attributes without ATTR_VOL_INFO;
    - document ids for UF_TRACKED items and non-zero generation counts;
    - dir/file attributes packed by object type with FSOPT_PACK_INVAL_ATTRS;
  - kern.osproductversion is "26.4" in simulator processes;
  - xattr lists hide `x86code.bsdflags`.

Installed without sudo (copy + rename, scratch boot.sh): darlingserver, libsystem_kernel.

Other changes:
- host/simfb/simfb.c: shared-memory ring, present FIFO, statistics, framebuffer.bgra every 2 s.
- tools:
  - simview.py: GTK 4;
  - simstream.py: ring, 60 fps, 0.75 s HLS;
  - progress_site.py and fb2png.py read the ring;
  - fbring.py and fpsmeter.py are new.
- cli/libexec/rundev.sh adds syncdefaultsd, amsaccountsd and com.x86code.fpsim2host.
- cli/libexec/run-launchd-sim.sh: /etc/localtime.
- host/simshim/compat/simshim.c: sim2host skip for com.apple.FileProvider.sim2host.
- Screens 78-83.

## Spike 3.14 — Preview opens documents from Files, FSEvents, revisiond, host file services, runtime setup (2026-10-28)

**Results:**
- **Files hands documents to Preview.** A PNG tapped in Files (or in Files' search results) opens in Preview (87).
  Files' On My iPhone lists the folder normally afterwards, with the PNG's last-opened time (88): the
  "Content Unavailable" after the hand-off is gone.
- **Files gets live updates.** A file written from Linux into the On My iPhone folder appears in the open Files window
  within a second (86). host/fseventsd serves com.apple.FSEvents from Linux inotify.
- **fpsim2host answers all 16 FPSimulatorSupport methods** (tests/probes/s2hprobe).
- **The SpringBoard MTLSimDevice crash (open since 3.12) is explained and fixed** (vkmtl's scheduled/completed order).
- **App survey:** 18 apps open and draw, no app crashes (89).
- **Runtime setup is reproducible:** tools/runtime/extract-runtime.py; a fresh extraction matched the working tree.
- The boot that "died at startup" was not reproduced in about 25 boots; one boot hung black under heavy disk load
  (below).

Prior work checked first: research/willi, VibeDarling and linuxkid473 have nothing on FSEvents/fseventsd, openbyid_np,
GenerationalStorage/revisiond, fsgetpath of document ids or RLIMIT_NOFILE with mldr's RPC sockets.

### Preview couldn't resolve "did=" items: three missing pieces
Preview logged `Error resolving bookmark; bookmark: fp:/.../com.apple.FileProvider.LocalStorage//did=791935814;
error: ... -1005 ... NSFileProviderResolverErrorDomain -1012`; Files later logged "Got a nil url from fetchURL for item
... -1005", fileproviderd "Cannot get URL for item ID localstorage/did=...", and the On My iPhone extension
"can't resolve did=791935814 [70: Stale NFS file handle]". Each fix exposed the next:

1. **FSEvents** (`FSEventsCopyUUIDForDevice: FSEvents_connect() => Unknown service name`): the extension had no
   volume UUID and no live updates. New host/fseventsd (below).
2. **revisiond wasn't running.** `did=` ids are document ids (GenerationalStorage). `GSLibraryResolveDocumentId2`
   asks revisiond (`com.apple.revisiond`, `-[GSClient getFileIdForDocumentId:onDevice:reply:]`) for the file id, then
   `openbyid_np(fsid, fileid)` and checks the document id of what it opened. With no revisiond, the reply block was
   never called, file id 0 was opened (ENOENT), and the extension reported ESTALE. rundev now starts
   com.apple.revisiond (a LaunchDaemon of the runtime, RunAtLoad/KeepAlive on a Mac's simulator). Then:
   - revisiond aborted at its first thread: mldr printed **"Failed to create socket"**. revisiond sets
     RLIMIT_NOFILE to 1000; mldr gives every thread its own darlingserver socket and puts those at the top of the
     *initial* fd range (socket_bitmap_get, from 1048575 down), so `dup2` to the high fd failed with the lowered limit.
     Fix (darling-xnu setrlimit.c): a request below the current Linux limit succeeds and changes nothing. (Recording
     the lower value for getrlimit made notifyd abort; notifyd's own request, {10240, RLIM_INFINITY}, had always
     failed with EPERM on Linux, which it ignores.)
   - revisiond: `fstatfs(58) failed; error 2`, "Failed creating library for 58": it opens the volume root as
     `openbyid_np(fsid 58, object id 2)` (2 = an APFS/HFS root). **openbyid_np (BSD 479) didn't exist.** New in
     misc/fsgetpath.c: resolved like fsgetpath, then opened by path; object id 2 is the mount point, found from any
     path of that device the shared id table knows (walking up while the parent has the same st_dev).
   - Document ids are the inode's low 32 bits (3.13); getattrlist now remembers them in the fsgetpath table too, and
     fsgetpath accepts a 32-bit id that matches the inode's low bits.
   - Result: revisiond logs `getFileIdForDocumentId:... reply(791935814, ...)` and "Created new entry for
     </Containers/Shared/AppGroup/.../File Provider Storage/Notes from Linux.txt dev:3a docID:...>"; its library lives
     in the device (paths are relative to the device's home), nothing is written to Linux's /home.
3. Preview then opened the PNG (87).

Tools: tests/probes/docidprobe (getattrlist document id, openbyid_np by it, statfs fsid), host/simshim/diag/attrdiag
also logs openbyid_np/fsgetpath/`/.vol` opens.

### host/fseventsd: com.apple.FSEvents from inotify
- **Protocol** ([protocols/FSEVENTS.md](../protocols/FSEVENTS.md)), reconstructed from the runtime's FSEvents.framework
  MIG stubs: f2d 66000 register (5 descriptors: client port, path offsets/blob, exclusion offsets/blob; dev, flags,
  latency, sinceWhen), 66001 flush, 66002 server UUID, 66003 current id, 66004 last id before time, 66005 purge,
  66006 unregister; d2f 67000 callback (offsets, raw flags, ids, path blob). The raw flag bits and their mapping to
  FSEventStreamEventFlags come from `implementation_callback_rpc`.
- **inotify from a simulator process.** Simulator processes link the runtime's libsystem_kernel; Darling's
  `__linux_inotify_*` exports are out of reach. New Darling-private syscall 598 (darling-xnu x86code_inotify.c):
  init1, add_watch (vchroot-expanded path), rm_watch; events are read() from the Linux fd.
- **Watches.** Every stream path is watched with its tree; new directories get watches as they appear; a missing
  path is watched through its nearest existing ancestor. A stream on "/" (searchd/corespotlight, 6 exclusions) first
  walked all of Linux through /Volumes/SystemRoot (30,000 watches, then a hang); streams on an ancestor of the device
  now watch the device's data directory (about 9,500 directories). The watch table is indexed by watch descriptor.
- **Clients at boot:** cfprefsd (Preferences directories, file events), profiled, nsurlsessiond, navd, MobileAsset,
  searchd, and LocalStorageFileProvider (volume UUID, short "since" streams for catch-up, file-event streams for the
  open folder).
- **History:** none. Event ids persist across boots (a lease file), the volume UUID too; a stream starting at an older
  id gets MustScanSubDirs for its paths, then HistoryDone (the catch-up streams unregister right after it).
- **Client lifetime:** the client's port dying stops events; the stream goes when its port has no senders or on
  unregister. (Destroying the stream on the dead-name notification made the client's own unregister fail with
  "f2d_unregister_rpc() => (ipc/mig) server died (-308)".)

### fpsim2host: the remaining 8 methods
What the Mac's fileproviderd does for each (from FileProvider's fpfs_* callers in the runtime) and what we do:

| Method | Mac host | Here |
|---|---|---|
| pkgLookup | package detection on the host path | FileProvider's own `fpfs_pkg_path_lookup` on the path (was: always "not a package") |
| dirEntryHelper | next directory entries without materializing | getattrlistbulk records (≤ 32 KB, with ATTR_CMN_RETURNED_ATTRS) and their count; the shared fd continues |
| makeDatalessFileAt | APFS dataless item (SF_DATALESS, logical size) | SF_DATALESS via fchflags (Darling keeps BSD flags in an xattr) + sparse file of the size |
| unsetDatalessBitAt | clears SF_DATALESS | same |
| materializeFile | the downloaded content becomes the file's data | copied in, flag cleared |
| markPurgeable | APFS purgeable marks | kept in an xattr (nothing purges) |
| setSyncRoot | APFS sync root | kept in an xattr (Darling's fsctl still answers "no sync root") |
| register/unregisterMaterializationHandler | handler called on a dataless fault | kept; never called (no faults) |

The completion block's integer slots now take separate values (pkgLookup's (BOOL, BOOL, u64, error)).
tests/probes/s2hprobe calls each over XPC as a job (`x86sim boot "com.x86code.s2hprobe=tests/probes/s2hprobe|<dir>"`):
all answer; makeDataless gives size 12345 with flags 0x40000000, materialize gives the content with flags 0.
Package detection with detection 0 (what LocalStorage passes) reports .app/.bundle directories without the bundle bit
as plain directories.

### SpringBoard SIGSEGV in -[MTLSimDevice commandBufferForReference:] (open since 3.12)
- Seen again in boot 5 of the first loop (twice in a row, 15:11:09 and 15:11:20). tools/debug/sigctx.py: the fault is
  in the block of `commandBufferForReference:`, which takes `unordered_map::find(ref)->second` without checking for
  end(). handleMainConnectionReplies calls it for message 100 (scheduled) and 101 (completed).
- Both messages come from Apple's MTLSimImplementation inside metalhostd, driven by our VKMTLCommandBuffer's
  handlers. `commit` sent the command buffer to mtlvk and *then* ran the scheduled handlers; the completion arrives
  on another queue after ~10 ms, so a preempted commit thread let the completed handlers run first. The client got
  101 (and released the ref), then 100 for a ref it no longer had.
- Fix (host/metalhostd/vkmtl.m): scheduled handlers run before the command buffer is sent. `x86sim probe metal` 46/46.

### Runtime setup (README step 2) checked against how it was done
- Spike 0 and "Linux-side extraction": the universal runtime comes from `xcodebuild -downloadPlatform iOS
  -buildVersion 26.5 -architectureVariant universal -exportPath ~/simrt` on a Mac (the installed runtimes are
  arm64-only), and `7z x -snld` of `Restore/iOSSimulatorRuntime_Cryptex.dmg` on Linux. The README said to copy the
  installed `.simruntime` from a Mac, which gives the arm64-only runtime.
- 3.4: 80 links had come out as empty files and were restored from a Mac by hand (restored-symlinks.tsv).
  7-Zip 26.05 instead skips 6 ("Dangerous link via another link was ignored", exit code 2), and `-snld` writes
  absolute links (/System/...) relative to the archive root, i.e. broken.
- tools/runtime/extract-runtime.py does the extraction and then makes each of the image's 317 links again from
  `7z l -slt` (absolute ones relative inside RuntimeRoot). `--check` compares a tree with the image. A fresh
  extraction (12 min) matched the working tree in names, sizes, types and link targets (727,870 entries), except four
  dangling TTS voice links that are now relative too.
- `make help`, `make mtlvk`, `make darling` (existing build dir) and a fresh cmake configure of external/darling work;
  `make darling-install` needs sudo (not run).

### Boots
- About 25 boots this spike (two loops of 5 and 6 boots, the rest while testing): no boot where every process was
  signalled at once. The second loop (after the vkmtl fix) had no crash of any process in 6 boots.
- First loop, boot 2 (during the 7z extraction, nice/ionice'd): SpringBoard and backboardd spawned, no frame after
  4 min. Nothing in their logs; mtlvk's log was overwritten by the next boot. `x86sim boot` now keeps the logs of a
  boot that doesn't reach the home screen in `$X86SIM_SCRATCH/failed-boot-<time>`.

### Files: navigation back crashed (seen only before the fixes)
- While "did=" resolution failed, going back from a tag view in Files (back button or the Browse tab) crashed Files
  every time (SIGSEGV in objc_msgSend from `-[_UIViewControllerTransitionContext _runAlongsideCompletions]`, a
  released object in a transition's alongside-completion), and DocumentManagerUICore.Service crashed going back in
  Preview's browser. After revisiond and openbyid_np, the same back navigation works. Not investigated further.

### Open (next)
- fseventsd has no event history; the latency isn't applied.
- Dataless files don't fault (no materialization on read).
- A boot hung black once under heavy disk load (logs now kept).
- Settings row icons were colored in this spike's boots (89); the gray-icon issue of 3.8 was not seen.
- From 3.12: suggestd (left out), the Game Center icon.

Darling changes in this spike (darling-xnu):
- syscall 598: inotify for simulator processes (x86code_inotify.c);
- openbyid_np (479); fsgetpath/openbyid of document ids and of object id 2 (the volume root);
- setrlimit(RLIMIT_NOFILE) below the current limit succeeds without changing it.

Installed without sudo (copy + rename into darling-prefix): libsystem_kernel.

Other changes:
- host/fseventsd (new), build/mac-build-fseventsd.sh, rundev jobs com.x86code.fseventsd and com.apple.revisiond;
- host/fpsim2host: all methods; tests/probes/s2hprobe, docidprobe; attrdiag by-id logging;
- host/metalhostd/vkmtl.m: scheduled before commit;
- cli/x86sim: failed-boot log snapshot;
- tools/runtime/extract-runtime.py, README setup; docs/protocols/FSEVENTS.md;
- Screens 86-89.

## Spike 3.15 — Your own apps: x86sim install / launch, sample apps, Metal apps on CAMetalLayer (2026-10-28)

**Results:**
- **`x86sim install <path.app>`, `x86sim uninstall <bundle id>`, `x86sim launch <bundle id> [args]`** work like
  `xcrun simctl install/uninstall/launch`. An installed app gets its bundle under `data/Containers/Bundle/Application`,
  a data container, a LaunchServices record and a home-screen icon (90, 97); it opens from `launch` or by tapping it.
- **Four sample apps** (tests/apps, built on the Mac without an Xcode project by build/mac-build-apps.sh, x86_64,
  ad-hoc signed) run: UIKit with touch (91, 92), SwiftUI with a list, navigation, an animation and an HTTPS fetch
  (93, 94), MetalKit (MTKView render loop, shader compiled at run time, presented through CAMetalLayer on our Metal
  path, 60 fps; 95) and SpriteKit (60 fps; 96). No change to the Metal path was needed for CAMetalLayer drawables.
- **An Apple sample** (CloudKit "Queries", github.com/apple/sample-cloudkit-queries, built with xcodebuild
  `ARCHS=x86_64 -sdk iphonesimulator`) installs and runs; it reports "requires an authenticated account", as on a
  Mac's simulator without an iCloud login (98).
- **tests/apps.sh** (`x86sim apps`): install, launch, pixel and animation checks for each app, uninstall + reinstall
  and opening by tapping the icon. 16/16 pass.
- The device now has the iPhone 16 device type's capabilities: the Dynamic Island, the Watch app and the iPhone 16
  dock appear as on a Mac's iPhone 16 simulator.

Prior work checked first: research/willi, VibeDarling and linuxkid473 have nothing on installd, InstallCoordination
or simctl install.

### How simctl install works on a Mac
CoreSimulator's `-[SimDevice installApplication:withOptions:...]` asks **CoreSimulatorBridge**, a job inside the
device (`.../CoreSimulator.framework/Resources/Platforms/iphoneos/usr/libexec/CoreSimulatorBridge`, host-side
`host_support_mig_install_app`). The Bridge calls `+[IXAppInstallCoordinator installApplication:consumeSource:options:
completion:]` with `MIInstallOptions` (`setDeveloperInstall:`, `setInstallTargetType:`) and carries
`com.apple.private.mobileinstall.allowedSPI = [InstallForLaunchServices, UninstallForLaunchServices]`. Uninstall is
`+[IXAppInstallCoordinator uninstallAppWithBundleID:requestUserConfirmation:...]`. host/openurl now does the same
(`install <path>`, `uninstall <id>` lines; `app <id>\t<arg>...` passes `__Arguments` to FrontBoard).
`-[LSApplicationWorkspace installApplication:withOptions:error:]` answers -4 (unimplemented) on iOS 26.

### Four failures on the way
1. LSApplicationWorkspace: -4 (above).
2. `IXErrorDomain 4 "This SPI can only install apps"`: IXSimpleInstaller checks `options.installTargetType == 1`
   (InstallCoordination disassembly; selref resolved to `installTargetType`). Set it.
3. `getattrlist failed ... Invalid argument` in `-[MIFileManager copyVolumeInfo:forURL:error:]`
   (installcoordinationd): the attrlist (from InstalledContentLibrary's constant) is volattr 0x00141000 =
   MOUNTPOINT|UUID|**FSTYPENAME**, without ATTR_VOL_INFO. Darling's VALIDMASK stopped at 0x0007FFFF. Darling now
   answers the newer volume attributes (MOUNTEXTFLAGS, FSTYPENAME = statfs's "apfs", FSSUBTYPE, OWNER, SPACEUSED).
4. `setattrlist failed` in `-[MIFileManager _bulkSetPropertiesForPath:...removeACL:]` (installd's staging):
   ATTR_CMN_EXTENDED_SECURITY (an empty ACL) was EINVAL. Accepted and ignored now, with ATTR_CMN_UUID/GRPUUID.

### The installed app aborted: no device-type capabilities
The first launch died with SIGSEGV (si_code SI_USER) in `_Unwind_RaiseException`; tools/debug/sigctx.py showed an
NSException thrown from UIKitCore. liboslogdiag on the app gave the reason: CoreUI "couldn't get
'kMGArtworkDeviceIdiom' from MGCopyAnswer" and `Assertion failure in -[_UIScreenComplexBoundingPathUtilities
_loadBitmapForScreen:type:]`. libMobileGestalt in the simulator reads the device type's capabilities from the plist
named by `SIMULATOR_CAPABILITIES` (CoreSimulator: `iPhone 16.simdevicetype/Contents/Resources/capabilities.plist`);
we set none, so only the `SIMULATOR_MAINSCREEN_*` answers existed. The runtime's own apps never reached that path.
mksimdevice.py now writes `<device>/capabilities.plist` from cli/lib/devicetype-iphone17,3.json (the iPhone 16
capabilities, taken from the Mac) and sets `SIMULATOR_CAPABILITIES`. Side effects, all as on a Mac's iPhone 16: the
Dynamic Island, the Watch app on the home screen, the floating dock.

### Open
- simctl's other app commands: `get_app_container`, `install_app_data`, `launch --console` / environment.
- The rest of the CoreSimulator device-type profile (framebuffer mask, chrome) is unused.
- From 3.14: FSEvents history/latency, suggestd, Game Center icon, widget extensions killed in the background, the
  black-screen hang under heavy disk load.

Darling changes in this spike (darling-xnu): getattrlist newer volume attributes; setattrlist ACL/UUIDs accepted.

Other changes: host/openurl install/uninstall/launch arguments (+ entitlements), cli/libexec/appinstall.sh,
`x86sim install|uninstall|launch|apps`, rundev job com.apple.installcoordinationd, device capabilities, tests/apps
(4 apps), build/mac-build-apps.sh, tests/apps.sh, screens 90-98.

## Spike 3.16 — Developer workflow (simctl parity), LLDB debugging, FSEvents latency, leftovers (2026-10-28)

**Results:**
- **simctl's developer commands** (`cli/libexec/simctl.py`, requests to host/openurl as `req <id> <json>`):
  `x86sim launch` with arguments, environment (`SIMCTL_CHILD_NAME=value` and `--env`), `--console` (the app's
  stdout/stderr streamed to the terminal), `-w/--wait-for-debugger`, `--terminate-running-process`; `terminate`,
  `get_app_container <id> [app|data|groups|<group>]`, `listapps [--json]`, `pidof`, `push` (simulated remote
  notification, delivered to the foreground app: 99), `addmedia` (Photos library: 100), `log stream --app` (os_log of
  an app), `io screenshot` / `io recordVideo` (MP4 from the frame ring). `location set|clear` is accepted but apps
  don't get the location yet (below).
- **Debugging with LLDB**: `x86sim debug [--launch] <bundle id>` attaches Linux's lldb to the app through LLVM's
  debugserver running inside the Darling container. Breakpoints, continue/step, symbolic backtraces into the app,
  UIKitCore and the rest of the runtime work (README "Debugging"); expression evaluation does not.
- **tests/devtools.sh** with a new test app (tests/apps/DevTools): 16/16, three runs in a row in one boot.
- **FSEvents latency** applied (deferral, NoDefer, flush), tests/probes/fsevprobe 3/3.
- **Package detection**: .app/.bundle directories without a bundle bit are packages for FileProvider.
- **suggestd** runs in the standard boot again (no crash in the boots of this spike).
- **Two darlingserver aborts fixed** that took the whole container down (debugger page queries; a child checking in
  after its parent died).

Prior work checked first: research/willi, VibeDarling and linuxkid473 have nothing on simctl's commands, debugserver
or FSEvents latency. Darling's docs describe debugging with its own lldb build and an external debugserver
(docs.darlinghq.org, "Debugging"); our prefix has neither (its /usr/bin/lldb is an xcrun stub).

### How CoreSimulatorBridge does simctl's commands (Mac, Xcode 27.1)
`.../CoreSimulator.framework/Resources/Platforms/iphoneos/usr/libexec/CoreSimulatorBridge` (strings and entitlements):
- launch: `FBSOpenApplicationService` with the launch options under `__DebugOptions` (FBSOpenApplicationOptionKey
  DebuggingOptions): `__Arguments`, `__Environment`, `__StandardOutPath`, `__StandardErrorPath`, `__WaitForDebugger`,
  `__DisableASLR`. At the top level FrontBoard only takes `__Arguments` (our first `--console` attempt set the paths
  there and the app's stdout stayed /dev/null). `terminate_running_process` first terminates.
- terminate: `-[FBSSystemService terminateApplication:forReason:andReport:withDescription:completion:]` (and an RBS
  termination assertion); entitlement `com.apple.multitasking.termination`.
- push: `+[UNNotificationRequest requestWithIdentifier:pushPayload:bundleIdentifier:]` added with
  `addNotificationRequest:withCompletionHandler:` on a `UNUserNotificationCenter` for the bundle; entitlement
  `com.apple.private.usernotifications.notification-simulator`.
- addmedia: `PHPhotoLibrary performChangesAndWait:` with `creationRequestForAssetFrom{Image,Video}AtFileURL:`;
  `com.apple.private.tcc.allow` = kTCCServicePhotos.
- location: `CLSimulationManager` (clearSimulatedLocations, appendSimulatedLocation:, setLocationInterval:/Speed:/
  Distance:, startLocationSimulation); `com.apple.locationd.authorizeapplications`.
host/openurl now does each the same way and carries those entitlements in `__TEXT,__entitlements`.

### os_log for one app without stalling the boot
Logging stays off for the boot (no logd/diagnosticd). `--oslog` / `log stream --app` relaunches the app with
`X86CODE_OSLOG` and `X86CODE_OSLOG_DIR` in its debug-option environment. `DYLD_INSERT_LIBRARIES` given that way is
replaced by launchd's, so the diag shim couldn't be inserted per app; liboslogdiag's formatter
(host/simshim/diag/oslogdiag.m, `-DSIMSHIM_OSLOG`) is now compiled into libsimshim and turns on only for processes
with `X86CODE_OSLOG` in their environment. Messages go to `data/var/log/oslog.<prog>.<pid>.log`; `log stream`
follows them (`--match` filters).

### LLDB: debugserver inside Darling
- Xcode 27's debugserver is arm64/arm64e only. build/mac-build-debugserver.sh builds llvm-project's
  lldb/tools/debugserver Xcode project (sparse checkout of llvmorg-21.1.0) for macOS x86_64. Built for macOS 27 it
  failed in Darling's dyld: `Symbol not found: __ZNSt3__117bad_function_callD1Ev` (Darling's libc++ is older);
  `MACOSX_DEPLOYMENT_TARGET=12.0` links only symbols Darling's libc++ has (checked with nm against the prefix's
  libc++.1.dylib).
- `darling shell <debugserver> 127.0.0.1:<port> --attach=<pid>` joins the running simulator container; Linux lldb
  23 (CachyOS package) connects with `platform select remote-macosx` + `process connect` and reads the Mach-O files
  from Linux paths (`image search-paths add /Volumes/SystemRoot /`).
- **darlingserver abort 1**: the first session took the whole container down (every process SIGILL at the same
  second). darlingserver's core: `dtape_stub_log` abort in `mach_vm_page_range_query`, an unsafe stub that
  debugserver calls for each memory region (qMemoryRegionInfo dirty pages). It now reports every page resident
  (VM_PAGE_QUERY_PAGE_PRESENT), as `mach_vm_page_query` does.
- **debugserver crashed attaching** in about half of the attempts: SIGSEGV in `MachThread::ThreadDidStop`
  (sigctx.py on its core). Its log showed `SetDefaultArchitecture (cpu_type=0x00000007) => 0`: i386, for which this
  x86_64-only build has no plugin. `sysctl.proc_cputype` answered 0x1000007 for the same pid (a small macOS probe
  run in the container), so the type came from `MachProcess::GetMainBinaryCPUTypes`, which reads the main binary's
  header from dyld's image list: an app stopped at its first instruction (`__WaitForDebugger`) may not have one yet.
  The build patches `DNBArchProtocol::SetArchitecture` to fall back to x86_64 when there is no plugin for the type.
  (Two Darling fallbacks for the process architecture tried on the way were reverted.)
- `x86sim launch -w`: FrontBoard answers `openApplication` only once the app runs, so the helper no longer waits
  for that answer; the CLI finds the waiting process in /proc (`NSpid` = the pid inside Darling).

### darlingserver abort 2: a child checking in after its parent died
During launch/terminate cycles the container died again: darlingserver SIGSEGV in `dtape_semaphore_up(NULL)` from
`Process::notifyCheckin`. A child checking in signals its parent's fork-wait semaphore; the parent had died and its
semaphore was already destroyed (set to NULL) while the `Process` object was still referenced. Now skipped when NULL.

### FSEvents: latency
fseventsd logged `latency 0.000` for every stream. The register request's field at 0x8c is a u64 in microseconds
(1.5 s = 0x16e360), not an f64 (raw dump of the request). Streams now keep pending events until first event +
latency (NoDefer: the first event after a quiet period goes at once); `f2d_flush` sends them and answers the stream's
own last event id (answering the global current id made `FSEventStreamFlushSync` wait forever).
tests/probes/fsevprobe: deferred stream called back once at 1.50 s for a burst of 5 files, NoDefer at 0.01 s, a
30 s-latency stream delivered by FlushSync. History across boots is still not kept.

### Package detection without a bundle bit
`fpfs_pkg_path_lookup` (detection 0) said "not a package" for Thing.app and Thing.bundle even with the bundle bit set
by hand in the FinderInfo xattr, so a Darling FinderInfo change (tried, reverted) wouldn't help. fpsim2host stands in
for the Mac host, whose fileproviderd also uses LaunchServices' package types: a directory whose extension's UTType
(`typeWithFilenameExtension:conformingToType:public.directory`) conforms to com.apple.package is a package now
(Thing.app -> com.apple.application-bundle, Thing.bundle -> com.apple.generic-bundle; s2hprobe: package 1).

### Leftovers checked
- **suggestd**: started in 8 boots of this spike (as an extra job, then in the standard boot); no crash, no core.
- **Widget extensions killed in the background** (PassbookWidgets, ScreenTimeWidgetExtension): launchd_sim's log says
  "exited due to SIGKILL, ran for 0ms", but the spawn and exit lines are ~10 s apart ("ran for" is wrong under
  Darling). chronod (the widget host) launches them for timelines and RunningBoard ends them when idle; the widgets
  render (Maps, Calendar on the home screen). Not a bug as far as seen.
- **Location simulation**: requests reach locationd's simulation service (`[Simulation] received daemon-side request
  to start location simulation`, once host/openurl ran its main run loop: CLSimulationManager's proxy is on the
  main queue) but not reliably, and no location reached an app. Open.
- **Game Center icon** (gray wireframe in Settings) and the **black-screen hang under heavy disk load** were not
  worked on.

### Open
- Location simulation (above); `--console` uses files (no pty); expression evaluation in LLDB.
- FSEvents history across boots; Game Center icon; heavy-disk-load hang.

Darling changes in this spike (darlingserver): `mach_vm_page_range_query`/`mach_vm_page_query` answered; fork-wait
semaphore of a dead parent.

Other changes: host/openurl (requests, entitlements, run loop), cli/libexec/simctl.py, simdebug.sh, libsimshim with
the os_log formatter, host/debugserver (build/mac-build-debugserver.sh), host/fseventsd latency, fpsim2host package
types, tests/apps/DevTools, tests/devtools.sh, tests/probes/fsevprobe, suggestd in rundev, screens 99-100.
