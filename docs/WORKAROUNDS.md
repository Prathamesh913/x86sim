# Workarounds vs faithful fixes

Every change in this project is either a **faithful fix** (Darling or a stand-in now behaves as XNU / CoreSimulator /
the Mac host does) or a **workaround** (something is skipped, faked or approximated so the simulator gets further).
This list is about the workarounds; the faithful fixes are the bulk of the Darling branches and are described in
[notebook/FINDINGS.md](notebook/FINDINGS.md). Each entry says where it lives and what a faithful version would need.

Legend: **W** = workaround, **S** = stand-in for a Mac/CoreSimulator component (faithful in intent, partial in
coverage), **F** = faithful fix listed for contrast because it replaced an earlier workaround.

## Stand-ins for the Mac host and CoreSimulator (S)

| What | Where | Coverage / gap |
|---|---|---|
| CoreSimulator's launchd handshake (bootstrap port, service port, holding tank) | host/simhost | Only MIG 708 and the holding-tank child; other requests are logged and dropped. |
| SimFramebuffer.framework (display) | host/simfb | One built-in display, no external displays, no host compositor. Frames are copied (not shared) into the ring. |
| SimulatorHID.framework (input) | host/simhid | Touchscreen (1-2 fingers), buttons, keyboard over a socket; no Indigo protocol, no accelerometer/pencil/game controllers. |
| MTLSimDriverHost / SimMetalHost (GPU) | host/metalhostd + linux/mtlvk + metal2vulkan | The runtime's own MTLSimImplementation on a Vulkan-backed device; feature coverage is what metal2vulkan translates (see METAL.md). |
| SimRenderServer's com.apple.IOSurface.Remote | host/iosurfaced | Surfaces as shared anonymous memory; no compression, no GPU-private surfaces. |
| Host mDNSResponder | linux/mdnsd | Unicast DNS (GetAddrInfo, QueryRecord). Register/Browse/Resolve are accepted but return nothing (no multicast DNS, no Bonjour). |
| Host configd (network state for configd_sim's SimulatorSupport) | linux/hostnet + host/netconfig + com.x86code.host-configd | A snapshot of the default-route interface at boot; not updated when Linux networking changes. |
| Host fileproviderd (com.apple.FileProvider.sim2host) | host/fpsim2host | All 16 methods. Dataless files are SF_DATALESS + a sparse file of the logical size: nothing faults on read (materialization handlers are kept but never called); purgeable and sync-root marks are only recorded (xattrs). |
| Host fseventsd (com.apple.FSEvents) | host/fseventsd + Darling syscall 598 (inotify) | Live events for registered paths (trees watched with inotify), volume UUID, persistent event ids. No event history across boots (MustScanSubDirs + HistoryDone instead); latency not applied; streams on `/` watch only the device's data directory. |
| `simctl openurl` / `simctl launch` | host/openurl (com.x86code.openurl) | Opens through FrontBoard's open-application service, as lsd does after choosing an app; carries the entitlements SpringBoard checks. |
| `simctl privacy grant` / provisioned test device | cli/libexec/seedperms.py | Writes TCC, location and notification answers into the device for the apps tested. |
| Host /System/Library/Frameworks lookups by absolute path | run-launchd-sim.sh (CoreSpotlight link into the Darling prefix) | Only CoreSpotlight; others the simulator might open on the host path are absent. |
| Host time zone (/etc/localtime) | run-launchd-sim.sh | Links the container's /private/etc/localtime to Linux's zone. |
| Host macOS version seen by simulator processes | Darling sysctl_kern.c: kern.osproductversion = 26.4 for iOS-simulator processes | A fixed value; the real host is Linux. |

## Workarounds in the simulator processes (W)

| Workaround | Where | Why / faithful alternative |
|---|---|---|
| **Icon-layer fallback**: when `-[IFConcreteImage ICRIconLayer]` returns nil, return a CALayer with the flat CGImage | simshim.c | ICRIconLayer needs an RBDevice and layer data that iconservicesagent's software renderer does not produce, so the first SpringBoard showed gray icons. Faithful: iconservicesagent rendering layer data on the Metal stand-in. |
| **Dummy sandbox extensions**: `sandbox_extension_issue_*` return opaque tokens, `consume`/`release` accept any token | simshim.c | Darling has no `__mac_syscall("Sandbox")` (ENOSYS) and nothing is sandboxed. Faithful: a sandbox (MACF) implementation. |
| **os_log disabled**: `OS_ACTIVITY_MODE=disable` for every launchd job (mksimdevice.py) and, from simshim, for processes launched by SpringBoard/RunningBoard; `os_state_add_handler` returns dummy handles meanwhile | mksimdevice.py, simshim.c | No logd/diagnosticd: logd crashed every process in `firehose_buffer_create` (Mach memory entries); without a draining diagnosticd, DT-mode senders block or flood lookups. `X86CODE_NODT=all,-<label>` / `x86code-stderr-progs` turn logging back on per process for debugging. Faithful: logd + diagnosticd running on Darling's memory entries. |
| **System-domain jobs loaded into the user domain** (`LimitLoadToSessionType: System` dropped) | mksimdevice.py | launchd_sim's system domain never leaves bootstrap mode here (on a Mac CoreSimulator ends it). Faithful: the missing bootstrap-completion step. |
| **Sim-to-host connections kept as simulator connections** (`xpc_connection_enable_sim2host_4sim` skipped for Metal, IOSurface, FileProvider host services) | simshim.c | Our stand-ins run inside the simulator on the simulator's libxpc, which drops macOS-format messages. Faithful: running them as macOS processes on the host libxpc (Darling's), which would need the sim-to-host XPC path. |
| **System power** registration that never fires | simshim.c | Darling's iokitd has no IOPMrootDomain; the device never sleeps. |
| **csops entitlement blobs** go to Darling's syscall instead of libsystem_sim_kernel's | simshim.c | The runtime's version fails a short buffer with ENOENT instead of ERANGE. Faithful fix would be in the runtime (not ours) or a matching kernel answer. |
| **Siri session language** pre-set (`com.apple.assistant.backedup` "Session Language" = en-US) | mksimdevice.py | assistantd would set it on a Mac's device; without it AFPreferences recomputes in a loop on the main thread. |
| **Permission prompts answered by OCR** (`x86sim dismiss`: tesseract finds "Allow" buttons and taps them) | simtouch.py | For prompts seedperms.py does not pre-answer. Test convenience only. |
| **Daemons left out of the standard boot**: suggestd (SIGSEGV at boot); logd, diagnosticd (see above) | rundev.sh | Each is an open bug, not a design choice. (revisiond was missing too and is now started: FINDINGS 3.14.) |
| **The standard boot lists its jobs** (about 70 labels in rundev.sh) instead of loading every LaunchDaemon of the runtime | rundev.sh | Jobs are added as apps need them, each with the reason in a comment. A Mac's device loads the whole set. |
| **Fresh container per boot**: killsim.sh kills every Darling process, then `darling shutdown` | run-launchd-sim.sh | Leftover processes and two launchd_sims on one device break new boots; killing processes inside a running container leaves darlingserver in a state where new jobs hang. |
| **statfs reports `apfs`** for local Linux file systems | Darling (libsystem_kernel statfs conversion) | FileProvider only accepts APFS volumes (the iOS simulator only runs on APFS). |
| **chflags flags stored in an xattr** (`user.x86code.bsdflags`, hidden from listxattr) | Darling (helper/misc/bsdflags.h) | Linux has no BSD file flags; this keeps them per file across processes. |
| **fsctl answers APFS requests as for a plain directory tree**; unknown ones ENOTTY | Darling (misc/fsctl.c) | Enough for FileProvider's sync-root and dir-stat queries. |
| **O_SYMLINK on regular files/directories reopens through /proc/self/fd** | Darling (fcntl/openat.c) | Linux O_PATH descriptors can't be read; XNU's O_SYMLINK only differs for symlinks. |
| **setrlimit(RLIMIT_NOFILE) below the current limit succeeds and changes nothing** | Darling (misc/setrlimit.c) | mldr keeps each thread's darlingserver socket at the top of the process's initial fd range; a lowered limit made thread creation abort (revisiond). Faithful: Darling's internal fds below the limit, or a limit enforced only on the process's own fds. |
| **Document ids are the inode's low 32 bits**; fsgetpath/openbyid_np resolve ids (and object id 2, the volume root) only from paths some process got from getattrlist | Darling (getattrlist_generic.c, misc/fsgetpath.c) | Linux can't open by inode number. A file nobody has stat'ed through getattrlist since its id was handed out cannot be found by id. |
| `NOTE_WL_SYNC_IPC` knotes accepted as no-ops; `memorystatus_control`, `kqueue_workloop_ctl` succeed | Darling (darling-xnu, libkqueue) | No memory pressure / jetsam, no sync-IPC priority inheritance. |
| `SIOCGCONNINFO` left ENOTTY | Darling (sockets) | Answering it made Network.framework abort on a recursive lock. |
| Runtime symlinks that 7-Zip skips or writes empty | tools/runtime/extract-runtime.py | Made again from the image's own listing after extraction (README setup step 2). |

## Diagnostics that change behaviour only when enabled (opt-in)

`host/simshim/diag/*` (via `X86CODE_INSERT`), `X86CODE_SHIMLOG`, `X86CODE_JOBLOG` (default on in rundev),
`X86CODE_KEEPALIVE`, `MTLVK_LOG`, `MTLVK_DUMP_PASSES`/`MTLVK_DUMP_REPLACE`, `X86CODE_VKMTL_LOG`/`X86CODE_VKMTL_DUMP`,
`X86CODE_IOSURFACE_LOG`, `X86CODE_MDNS_LOG`. They are not workarounds but are listed so they are not mistaken for
required parts.

## Replaced workarounds (F, for history)

- `xpc_init_services` was skipped in simshim (launchd_sim never answered the pid-domain uncork); removed once the
  underlying Darling bugs were fixed.
- Settings' gray row icons were first blamed on Metal; they also happen with software rendering and remain open
  (in-process IconServices store lookup misses; FINDINGS "Settings row icons are gray ... (not Metal)"). Not seen
  in the boots of 3.14 (colored rows).
- The workloop interposer (`host/simshim/diag/workloop_shim.c`) predates kernel workloops in Darling; it is kept for
  reference and not used.
- simfb's fixed 40 ms delay before copying a frame was replaced by waiting for GPU completion.
