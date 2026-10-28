# Architecture

x86Code runs Apple's **iOS 26.5 (23F73) x86_64 simulator runtime** on x86-64 Linux. The runtime's binaries are
unchanged; they run under **Darling** (a Darwin compatibility layer: Mach-O loader + XNU syscall/Mach emulation +
`darlingserver`). Everything CoreSimulator and the Mac host would provide around the runtime (the launchd handshake,
the display, touch input, GPU, cross-process surfaces, DNS, network state, host file services) is replaced by small
programs in this repo: some run **inside Darling** as simulator binaries (`host/`), some run **natively on Linux**
(`linux/`). The `x86sim` CLI (`cli/`) wires them together.

```
 Linux host                                           Darling container (one per boot)
 ---------------------------------------------        -------------------------------------------------------------
 x86sim (cli/x86sim)                                   darling shell -> simhost (host/simhost)
   rundev.sh: mtlvk, mdnsresponder, mksimdevice,          -> launchd_sim (runtime)  [device: $X86SIM_DEVICE/data]
              seedperms, run-launchd-sim.sh                  -> the runtime's daemons and agents (backboardd, SpringBoard,
                                                                cfprefsd, lsd, runningboardd, ..., apps)
                                                             -> our jobs: metalhostd, iosurfaced, netconfig,
                                                                host-configd, fpsim2host, fseventsd, openurl, prefs
                                                          every simulator process: DYLD_INSERT libkqueue_sim (Darling)
                                                                                   + libsimshim (host/simshim/compat)

 mtlvk (linux/mtlvk, Vulkan)   <== Unix socket + memfds ==  metalhostd (MTLSimImplementation on vkmtl)
 mdnsresponder.py (linux/mdnsd) <== dnssd_ipc socket ======  libsystem_dnssd in every process
 hostnet.py (linux/hostnet) --plist--> netconfig -> host-configd store -> configd_sim's SimulatorSupport
 simview.py / simstream.py / fb2png.py  <== /dev/shm ring ==  SimFramebuffer.framework (host/simfb) in backboardd
 simview.py / simtouch.py  == simhid.sock ==>                 SimulatorHID.framework (host/simhid) in backboardd
 darlingserver (Darling, Linux process): Mach IPC, threads, VM, memfd-backed shared memory for every container process
```

## Darling (external/darling, branch x86code)

Darling provides the Mach-O loader (`mldr`), the XNU syscall and Mach trap emulation (`libsystem_kernel`, in
`darling-xnu`), the Mach IPC/task/thread server (`darlingserver`), kqueue (`libkqueue`, plus `libkqueue_sim` injected
into simulator processes, whose libSystem is the runtime's), `dyld` for the host side and `iokitd`. The iOS
simulator runtime is far more demanding than the macOS userland Darling targets: workloops, cooperative thread pools
(Swift concurrency), ulock, Mach memory entries, guarded fds, getattrlist/fsctl/chflags, PF_ROUTE, csops and more.
All changes are on the `x86code` branches of our private forks ([REPOS.md](REPOS.md)), each marked `x86Code:` in the
source and explained in the lab notebook ([notebook/FINDINGS.md](notebook/FINDINGS.md)).

Inside Darling the Linux root is `/Volumes/SystemRoot`; the scripts translate paths (`mksimdevice.py darling()`).

## Booting: simhost and launchd_sim

`cli/libexec/mksimdevice.py` writes the device the way CoreSimulator does on a Mac: `<device>/data` (the device's
home), `LaunchDaemons/` and `LaunchAngels/` with only the requested jobs (copied from the runtime, or synthetic
`label=program` jobs for our binaries), `data/var/run/launchd_bootstrap.plist`, and `sim.env` (the `SIMULATOR_*`,
`DYLD_*` environment, UDID derived from the device path). `cli/libexec/run-launchd-sim.sh` restarts the container and
runs `host/simhost/simhost` inside it, which plays CoreSimulator's part of the launchd handshake (bootstrap port, the
service port MIG request 708, the holding-tank child) and execs the runtime's `launchd_sim`.
`cli/libexec/rundev.sh` (wrapped by `x86sim boot`) adds the standard job list, starts the Linux services first and
tails launchd_sim's log (in the Darling prefix: `$X86SIM_DPREFIX/private/var/tmp/com.apple.CoreSimulator.SimDevice.<UDID>/`).

## simshim (host/simshim/compat)

`libsimshim.dylib` is injected into every simulator process (`DYLD_INSERT_LIBRARIES` in `sim.env`). It interposes the
few host services a Linux host cannot provide: system power registration (no IOPMrootDomain), csops entitlement
blobs (goes to Darling's syscall), sandbox extensions (tokens issued and accepted; nothing is sandboxed), os_state
handlers while os_log is off, the icon-layer fallback (see [WORKAROUNDS.md](WORKAROUNDS.md)), and keeping
connections to our in-simulator stand-ins of Mac host services (Metal, IOSurface, FileProvider) plain simulator
connections instead of sim-to-host ones. Optional diagnostic shims (`host/simshim/diag`, `X86CODE_INSERT=...`) log
specific subsystems (sync XPC, entitlements, os_log to files, vnode sources, CloudKit, ...).

## Display: simfb and the frame ring

backboardd loads `SimFramebuffer.framework` from `SIMULATOR_FRAMEBUFFER_FRAMEWORK`. Ours (`host/simfb`) reports one
built-in display named `LCD` ([protocols/CADISPLAY.md](protocols/CADISPLAY.md)), gives CoreAnimation IOSurface
swapchains and copies each completed present into a shared-memory ring in Linux's `/dev/shm`
([protocols/SIMFB.md](protocols/SIMFB.md)). Linux readers: `simview.py` (GTK4 window), `simstream.py` (ffmpeg NVENC
H.264, 60 fps low-latency HLS into the site directory), `fb2png.py` (screenshots), `fpsmeter.py`, `framecheck.py`.

## Input: simhid

backboardd loads `SimulatorHID.framework` from `SIMULATOR_HID_SYSTEM_MANAGER`. Ours (`host/simhid`) adds virtual
touchscreen, button and keyboard HID services to backboardd's event system and feeds them from
`<device>/data/simhid.sock`, a line protocol ([protocols/SIMHID.md](protocols/SIMHID.md)) used by `simview.py` and
`simtouch.py`.

## GPU: metalhostd, vkmtl, mtlvk

On a Mac, simulator processes' Metal goes through `MTLSimDriver` over XPC to the host's `MTLSimDriverHost`, which
replays it on the Mac's GPU. Here `host/metalhostd` (a simulator job) loads the runtime's own `MTLSimImplementation`
and gives it a Metal device implemented by `vkmtl.m`, which forwards every object and command buffer over a Unix
socket (VKMTL protocol, [protocols/METAL.md](protocols/METAL.md)) to `linux/mtlvk`, a native Rust process that runs
it on Vulkan with **metal2vulkan** (`external/metal2vulkan`, branch x86code: Metal API on Vulkan + AIR to SPIR-V
translation). Buffer and texture memory is shared without copies: simulator memory is memfd-backed (darlingserver),
metalhostd obtains the fd with syscall 599 and passes it with `SCM_RIGHTS`; mtlvk imports it
(`VK_EXT_external_memory_host`). CoreAnimation's render server, the wallpaper and apps all use it
(`X86CODE_METAL=all`, default; `0` = software rendering). Details: [METAL.md](METAL.md).

## Cross-process surfaces: iosurfaced

IOSurfaces shared between processes (CoreAnimation layers, video, WebKit) are served on a Mac by CoreSimulator's
SimRenderServer as `com.apple.IOSurface.Remote`. `host/iosurfaced` implements that service inside the simulator;
surfaces are shared anonymous memory (memfds in darlingserver) handed out as `xpc_shmem`
([protocols/IOSURFACE.md](protocols/IOSURFACE.md)).

## Network: mdnsresponder and netconfig

The simulator resolves names through the host's mDNSResponder socket (`/var/run/mDNSResponder`).
`linux/mdnsd/mdnsresponder.py` implements the dnssd_ipc protocol on Linux (real DNS via dnspython) at
`$X86SIM_STATE/mDNSResponder`; `run-launchd-sim.sh` symlinks the container's `/var/run/mDNSResponder` to it.
Network state (what a Mac's configd publishes) comes from `linux/hostnet/hostnet.py` (the Linux default-route
interface as Setup:/State: keys), loaded by `host/netconfig` into a second `configd_sim` registered as
`com.apple.SystemConfiguration.configd` (`com.x86code.host-configd`), which the simulator's configd_sim mirrors. Sockets
and routing sockets themselves are Darling's (PF_ROUTE, socket options, EVFILT_SOCK).

## Host file services: fpsim2host, fseventsd, revisiond

FileProvider sends some operations to the Mac host's fileproviderd (`com.apple.FileProvider.sim2host`): open by file
id, openat/mkdirat/... on behalf of extensions, package lookup, directory entries, dataless files.
`host/fpsim2host` serves that interface inside the simulator (all 16 methods). Darling reports
`kern.osproductversion` 26.4 to simulator processes (the host macOS version the simulator expects).

File-system events come from the Mac's fseventsd (`com.apple.FSEvents`) on a real simulator. `host/fseventsd` serves
that MIG protocol ([protocols/FSEVENTS.md](protocols/FSEVENTS.md)) from Linux inotify, reached through Darling's
private syscall 598. The On My iPhone extension needs it for its volume UUID and for live updates in Files. Each
stream's latency is applied (events held, NoDefer, flush), as the Mac's fseventsd does. fpsim2host's package lookup
also takes directories whose extension is a registered package type (UTType) for packages, as the Mac host does.

Document ids (`did=` FileProvider items, bookmarks handed from Files to Preview) are resolved by the runtime's own
`revisiond` (GenerationalStorage), which opens the volume root by object id 2 and documents with `openbyid_np`;
Darling resolves both from the paths `getattrlist` has handed out (`fsgetpath.c`).

## Test helpers inside the device

`host/openurl` (job `com.x86code.openurl`) opens URLs and apps through FrontBoard (`x86sim openurl|app|launch`), reading
requests from `<device>/data/tmp/x86code-openurl`. It also installs and removes apps (`x86sim install|uninstall`,
`cli/libexec/appinstall.sh`), the way CoreSimulatorBridge does on a Mac: `+[IXAppInstallCoordinator
installApplication:consumeSource:options:completion:]` -> installcoordinationd -> installd (copies the bundle into
`data/Containers/Bundle/Application/<uuid>`) -> containermanagerd (data container) -> lsd (registration) ->
SpringBoard (icon). The device type's capabilities (iPhone 16, `cli/lib/devicetype-iphone17,3.json`) are written as
`<device>/capabilities.plist` and given to MobileGestalt with `SIMULATOR_CAPABILITIES`. `tests/probes/prefs` checks cfprefsd at each boot.
`cli/libexec/seedperms.py` pre-answers TCC/location/notification prompts like a provisioned test device.

simctl's developer commands (`cli/libexec/simctl.py`: `x86sim launch|terminate|get_app_container|listapps|push|
location|addmedia|log stream|io|pidof`) go to the same job as one-line JSON requests (`req <id> <json>`, answered by a
`result <id> <json>` log line). The job does what CoreSimulatorBridge does inside a Mac's device, with the Bridge's
entitlements: FrontBoard's debug options (`__DebugOptions`: arguments, environment, stdout/stderr paths for
`--console`, wait-for-debugger), `FBSSystemService` termination, `LSApplicationProxy` containers,
`+[UNNotificationRequest requestWithIdentifier:pushPayload:bundleIdentifier:]` for `push`, `CLSimulationManager` for
`location`, `PHPhotoLibrary` change requests for `addmedia`. `log stream --app` relaunches the app with
`X86CODE_OSLOG`, which turns on libsimshim's built-in os_log formatter (`host/simshim/diag/oslogdiag.m` compiled in):
messages go to `data/var/log/oslog.<prog>.<pid>.log` without logd/diagnosticd. `io recordVideo` encodes the frame
ring with ffmpeg.

## Debugging: debugserver and LLDB

`x86sim debug` (`cli/libexec/simdebug.sh`) runs LLVM's debugserver (built for macOS x86_64 from llvm-project sources by
`build/mac-build-debugserver.sh`, deployment target 12.0 for Darling's libc++) inside the simulator's Darling
container with `darling shell`; it attaches with Darling's ptrace/Mach task, thread and exception-port emulation.
Linux's own `lldb` connects to it over TCP (`platform select remote-macosx`, `process connect`) and reads the Mach-O
images from the Linux paths (`image search-paths add /Volumes/SystemRoot /`), so breakpoints and backtraces have the
app's and the runtime's symbols. `x86sim launch -w` starts the app stopped before `main` (FrontBoard's
`__WaitForDebugger`).

## CLI

`cli/x86sim` reads `x86sim.env` through `cli/lib/config.sh` (also used by every script; `cli/lib/x86config.py` for
Python) and dispatches to the scripts in `cli/libexec`, `tests/` and `tools/debug`. State of a boot (pids, sockets,
logs) is in `$X86SIM_STATE` (default `var/run`); large scratch files in `$X86SIM_SCRATCH`.

## Data flows at a glance

| Flow | From | Channel | To |
|---|---|---|---|
| frames | backboardd (simfb) | `/dev/shm/x86code-simfb.ring` | simview, simstream, fb2png, framecheck |
| input | simview, simtouch | `<device>/data/simhid.sock` (text lines) | backboardd (simhid) |
| Metal | every Metal client | XPC `com.apple.metal.simulator[.<proc>]` | metalhostd |
| Metal | metalhostd (vkmtl) | Unix socket `$X86SIM_STATE/mtlvk.sock` + memfds | mtlvk (Vulkan) |
| IOSurface | IOSurface.framework | XPC `com.apple.IOSurface.Remote` + xpc_shmem | iosurfaced |
| DNS | libsystem_dnssd | `/var/run/mDNSResponder` -> `$X86SIM_STATE/mDNSResponder` | mdnsresponder.py |
| network state | hostnet.py | plist -> netconfig -> host-configd | configd_sim (SimulatorSupport) |
| host files | FileProvider | XPC `com.apple.FileProvider.sim2host` | fpsim2host |
| file events | Linux inotify (syscall 598) | MIG `com.apple.FSEvents` (66000/67000) | FSEvents clients (cfprefsd, LocalStorageFileProvider, ...) |
| install app | x86sim install (appinstall.sh copies the .app into `data/tmp/x86code-install`) | `<device>/data/tmp/x86code-openurl` | openurl job -> InstallCoordination -> installd |
| open URL/app | x86sim openurl/app | `<device>/data/tmp/x86code-openurl` | openurl job -> FrontBoard |
| simctl commands | x86sim launch/push/... (simctl.py) | `req <id> <json>` in `<device>/data/tmp/x86code-openurl`, `result` lines in its log | openurl job -> FrontBoard, UserNotifications, Photos, CoreLocation, LaunchServices |
| debugging | lldb (Linux) | TCP 127.0.0.1:<port> (gdb-remote) | debugserver in the Darling container -> the app (Mach task/thread/exceptions) |
| stream | simstream.py | HLS in `$X86SIM_SITE/live` | http.server :8765 -> cloudflared |
