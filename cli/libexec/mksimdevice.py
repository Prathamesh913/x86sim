#!/usr/bin/env python3
"""Create a simulator device for launchd_sim under Darling.

Usage: mksimdevice.py <device dir> <job label | label=program>...

Writes <device dir>/data (the device's home, like CoreSimulator's
Devices/<UDID>/data), a LaunchDaemons directory holding only the given jobs
(copied from the runtime; label=program adds a RunAtLoad test job running a
host program, logging to data/var/log/<label>.log), launchd_bootstrap.plist and sim.env (the
environment launchd_sim is started with). The layout and keys mirror what
CoreSimulator produced on a Mac (research/macref/launchd_bootstrap.xml).
"""
import json
import os
import plistlib
import shutil
import sys
import uuid

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "lib"))
import x86config  # noqa: E402  (cli/lib/config.sh: repo root, Darling prefix, runtime, state dir)

PROJECT = x86config.ROOT
RUNTIME = x86config.get("X86SIM_RUNTIME")
# x86Code: jobs that stay eager (RunAtLoad/KeepAlive as Apple ships them); the rest start on first lookup (see below).
# sleepd: Apple ships RunAtLoad; boot-a64 std keeps it for cold-boot BKS/XPC stability. Without EAGER the
# slim-by-default pass below strips RunAtLoad and sleepd never starts under on-demand-only launchd_sim.
EAGER = {"com.apple.SpringBoard", "com.apple.backboardd", "com.apple.runningboardd", "com.apple.configd_sim",
         "com.apple.syslogd", "com.apple.UserEventAgent-System", "com.apple.distnoted.xpc.daemon",
         "com.apple.sleepd"}
PREFIX = x86config.get("X86SIM_PREFIX")
MTLVK_SOCKET = os.environ.get("X86CODE_MTLVK_SOCKET") or os.path.join(x86config.get("X86SIM_STATE"), "mtlvk.sock")
# x86Code: the host side of the simulator's audio (cli/libexec/simaudio.py, started by rundev.sh)
AUDIO_SOCKET = os.environ.get("X86CODE_AUDIO_SOCKET") or os.path.join(x86config.get("X86SIM_STATE"), "simaudio.sock")
AUDIO = os.environ.get("X86CODE_AUDIO", "0") != "0"  # opt-in until playback works (FINDINGS 3.18)
DARLING_VIEW = "/Volumes/SystemRoot"  # Linux / as seen inside Darling
# x86Code: X86SIM_ARCH=arm64: a device for the arm64 runtime (iOS 27.0, run under libarm64jit; ARM64-PLAN stage 2):
# arm64 device type (iPhone 17, iPhone18,3), arm64 builds of our in-simulator binaries (host/<name>/arm64/...; jobs
# and shims without one are left out), no libkqueue_sim (the arm64 process's libsystem_kernel is Darling's own, behind
# the svc thunks) and no x86_64 dyld cache. Unset: the x86_64 iOS 26.5 device, unchanged.
ARM64 = os.environ.get("X86SIM_ARCH") == "arm64"
DEVTYPE = {"model": "iPhone18,3", "class": "V57", "w": "1206", "h": "2622", "json": "devicetype-iphone18,3.json",
           "version": "27.0", "build": "24A434", "arch": "arm64"} if ARM64 else \
          {"model": "iPhone17,3", "class": "D47", "w": "1179", "h": "2556", "json": "devicetype-iphone17,3.json",
           "version": "26.5", "build": "23F73", "arch": "x86_64"}


# Speed 22: arm64 devices of other models. <device>/model names an entry of cli/lib/devicetypes-a64.json (Xcode 27.1's
# device types: identifier, product class, screen, capabilities); written by `x86sim device create --model`. No file:
# iPhone 17, as before.
DEVTYPES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "lib", "devicetypes-a64.json")


def device_type(device):
    if not ARM64:
        return dict(DEVTYPE, scale="3.000000", pitch="460.000000", suffix="iphone")
    try:
        name = open(os.path.join(device, "model")).read().strip()
    except OSError:
        name = "iPhone 17"
    with open(DEVTYPES) as f:
        dt = dict(DEVTYPE, **json.load(f)[name])
    # the runtime's own version (another iOS 27.x runtime via A64_RUNTIME)
    try:
        with open(os.path.join(RUNTIME, "System/Library/CoreServices/SystemVersion.plist"), "rb") as f:
            sv = plistlib.load(f)
        dt.update(version=sv["ProductVersion"], build=sv["ProductBuildVersion"])
    except (OSError, KeyError, plistlib.InvalidFileException):
        pass
    try:
        dt["name"] = open(os.path.join(device, "name")).read().strip() or "x86code"
    except OSError:
        pass
    return dt


def host_path(rel):
    """Linux path of one of our in-simulator binaries (rel: host/<name>/...); for arm64 host/<name>/arm64/..., or
    None if that build doesn't exist."""
    if not ARM64:
        return PROJECT + "/" + rel
    a, b, rest = rel.split("/", 2)
    p = "/".join([PROJECT, a, b, "arm64", rest])
    return p if os.path.exists(p) else None


def darling(path):
    return DARLING_VIEW + path


def runtime_climb_depth():
    """Number of /.. needed so xpcproxy_sim's access(RuntimeRoot + program) reaches /.

    RuntimeRoot is often a symlink into extracted/.../RuntimeRoot. access(2) walks the
    physical target before applying '..', so depth must use realpath. The arm64
    ios-27 symlink is 9 components long but 18 physically; with the short depth,
    host jobs (metalhostd, iosurfaced, mtlprobe) failed with exit 78 / ENOENT.
    """
    return len(darling(os.path.realpath(RUNTIME)).strip("/").split("/"))


def main():
    device = os.path.abspath(sys.argv[1])
    jobs = sys.argv[2:]
    udid = str(uuid.uuid5(uuid.NAMESPACE_URL, device)).upper()
    data = device + "/data"

    for d in ["tmp", "var/run", "var/db", "var/log", "Library/Preferences", "Library/Logs",
              "Library/Application Support/Simulator", "Library/LaunchDaemons"]:
        os.makedirs(os.path.join(data, d), exist_ok=True)
    open(data + "/var/run/memory_warning_simulation", "a").close()

    # x86Code: Siri's session language, as a Mac's simulator device has it (assistantd sets it there; it doesn't run
    # here). Without it AFPreferences logs "No language code saved, but Assistant is enabled" and recomputes the
    # language in a loop on the main thread: Spotlight and Safari spun at 100% CPU and Safari's typing stalled.
    ap = os.path.join(data, "Library/Preferences/com.apple.assistant.backedup.plist")
    try:
        with open(ap, "rb") as f:
            prefs = plistlib.load(f)
    except Exception:
        prefs = {}
    if "Session Language" not in prefs:
        prefs["Session Language"] = "en-US"
        with open(ap, "wb") as f:
            plistlib.dump(prefs, f, fmt=plistlib.FMT_BINARY)

    daemons = device + "/LaunchDaemons"
    shutil.rmtree(daemons, ignore_errors=True)
    os.makedirs(daemons)
    # x86Code: LaunchAngels (UI services launched on demand, e.g. PosterBoard, which draws the wallpaper) live
    # in their own directory; CoreSimulator hands launchd_sim that path too. arm64 (Speed 16): if
    # launchd_sim does not see them as angels, RunningBoard says osservice<com.apple.campo> is no angel, FrontBoard gives
    # Campo a plain FBProcess and SpringBoard's Siri scene transaction asserts ("process was nil").
    angels = device + "/LaunchAngels"
    if ARM64:
        # launchd_sim takes a plist's path from the open file (F_GETPATH: canonical) and makes the job an angel only
        # if it starts with <SimulatorRuntimeRoot>/System/Library/LaunchAngels/. So the device's angels go into its
        # own hidden subdirectory of the runtime's (real) LaunchAngels directory, and SimulatorRuntimeRoot (below)
        # is the canonical runtime path, as the cryptex mount is on a Mac.
        shutil.rmtree(angels, ignore_errors=True)
        angels = os.path.join(os.path.realpath(os.path.join(RUNTIME, "System/Library/LaunchAngels")),
                              ".x86code-" + os.path.basename(os.path.abspath(device)))
    shutil.rmtree(angels, ignore_errors=True)
    os.makedirs(angels)
    for job in jobs:
        if "=" in job:
            # x86Code: label=program[|arg|...|mach:<service name>|env:K=V]: extra arguments; mach: entries become MachServices
            label, program = job.split("=", 1)
            parts = program.split("|")
            program, extra = parts[0], parts[1:]
            # x86Code: env:K=V entries become EnvironmentVariables
            envs = dict(a[4:].split("=", 1) for a in extra if a.startswith("env:"))
            args = [a for a in extra if not a.startswith("mach:") and not a.startswith("env:")]
            mach = {a[5:]: True for a in extra if a.startswith("mach:")}
            log = darling(data + "/var/log/" + label + ".log")
            # xpcproxy_sim prefixes program paths with the runtime root: climb exactly back out of it
            depth = runtime_climb_depth()
            program = "/.." * depth + darling(os.path.abspath(program))
            job_plist = {"Label": label, "ProgramArguments": [program] + args,
                         "RunAtLoad": True, "StandardOutPath": log, "StandardErrorPath": log,
                         # X86CODE_KEEPALIVE=1: respawn every 10 s (to attach debuggers)
                         "KeepAlive": os.environ.get("X86CODE_KEEPALIVE") == "1", "ThrottleInterval": 10}
            if mach:
                job_plist["MachServices"] = mach
            if envs:
                job_plist["EnvironmentVariables"] = envs
            with open(os.path.join(daemons, label + ".plist"), "wb") as f:
                plistlib.dump(job_plist, f)
            continue
        src, dst = os.path.join(RUNTIME, "System/Library/LaunchDaemons", job + ".plist"), daemons
        if not os.path.exists(src):
            src, dst = os.path.join(RUNTIME, "System/Library/LaunchAngels", job + ".plist"), angels
        with open(src, "rb") as f:
            plist = plistlib.load(f)
        # launchd_sim's system domain never leaves bootstrap mode here (on a Mac something,
        # presumably CoreSimulator, ends it), so system-only jobs are loaded into the user
        # domain instead, where all lookups from the device's jobs resolve.
        if plist.get("LimitLoadToSessionType") == "System":
            del plist["LimitLoadToSessionType"]
        # x86Code: slim by default. launchd_sim starts a job with MachServices at the first lookup of one of them; what
        # RunAtLoad / KeepAlive (true, or SuccessfulExit) starts eagerly at boot runs whether anything uses it or not
        # (assistantd, locationd, identityservicesd, ids_simd, navd, revisiond, amsaccountsd: ~100 MB each
        # here). Those become on-demand, except what the home screen itself needs (EAGER). X86SIM_EAGER=1: Apple's.
        if os.environ.get("X86SIM_EAGER") != "1" and job not in EAGER and plist.get("MachServices"):
            plist.pop("RunAtLoad", None)
            ka = plist.get("KeepAlive")
            if ka is True or (isinstance(ka, dict) and "SuccessfulExit" in ka):
                del plist["KeepAlive"]
        # arm64: launchd_sim never leaves on-demand-only; SpringBoard is still spawned as "system support"
        # immediately while backboardd stays pending then speculative. If launchd_sim later aborts, SB's
        # late BKSHIDEventDeliveryManager init sees a nil connection and brk#0. Keep RunAtLoad for now
        # (home needs SB); sleepd EAGER above is the cold-boot BKS stabilizer. Optional delay: set
        # X86CODE_DELAY_SB=1 to drop SpringBoard RunAtLoad (caller must kickstart after backboardd).
        if ARM64 and job == "com.apple.SpringBoard" and os.environ.get("X86CODE_DELAY_SB") == "1":
            plist.pop("RunAtLoad", None)
        if os.environ.get("X86CODE_JOBLOG") == "1":
            # X86CODE_JOBLOG=1: job output and os_log (OS_ACTIVITY_DT_MODE) to data/var/log/<label>.log
            log = darling(data + "/var/log/" + job + ".log")
            plist.update({"StandardOutPath": log, "StandardErrorPath": log})
            # X86CODE_NODT=label,...|all: not for these (DT mode also streams every log line to
            # diagnosticd, and a sender blocks when diagnosticd stops draining)
            nodt = os.environ.get("X86CODE_NODT", "").split(",")
            # x86Code: "all,-label" keeps DT mode (os_log in the job's log) for that label only
            if ("-" + job) in nodt or (job not in nodt and "all" not in nodt):
                plist.setdefault("EnvironmentVariables", {})["OS_ACTIVITY_DT_MODE"] = "1"
            else:
                # no logd here either: without this libtrace streams to diagnosticd anyway
                plist.setdefault("EnvironmentVariables", {})["OS_ACTIVITY_MODE"] = "disable"
        # x86Code arm64 (Speed 13): assistantd is here only for com.apple.assistant.dictation (the keyboard's dictation
        # availability; see boot-a64). Its LaunchEvents and its other ~30 MachServices started it at boot, and it
        # then pulled in cloudd, nsurlsessiond, securityd and tccd. Serve dictation only. X86SIM_ASSISTANTD=full: Apple's.
        if ARM64 and job == "com.apple.assistantd" and os.environ.get("X86SIM_ASSISTANTD") != "full":
            plist.pop("LaunchEvents", None)
            keep = os.environ.get("X86SIM_ASSISTANTD_MACH", "com.apple.assistant.dictation").split(",")
            plist["MachServices"] = {k: True for k in plist.get("MachServices", {}) if k in keep}
        # x86Code: X86SIM_JOBENV="label:K=V;label:K=V": extra environment for runtime jobs (experiments)
        for kv in filter(None, os.environ.get("X86SIM_JOBENV", "").split(";")):
            lab, _, ev = kv.partition(":")
            if lab == job and "=" in ev:
                k, v = ev.split("=", 1)
                plist.setdefault("EnvironmentVariables", {})[k] = v
        with open(os.path.join(dst, job + ".plist"), "wb") as f:
            plistlib.dump(plist, f)

    rt, home = darling(RUNTIME), darling(data)
    # x86Code: iPhone 16 (iPhone17,3) device type capabilities, as JSON in cli/lib, written as a plist for MobileGestalt
    capabilities = device + "/capabilities.plist"
    dt = device_type(device)
    with open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "lib", dt["json"])) as f:
        caps = json.load(f)
    with open(capabilities, "wb") as f:
        plistlib.dump(caps, f)
    # Stand-in for the host's configd: configd_sim's SimulatorSupport plugin opens the
    # Mac's dynamic store (com.apple.SystemConfiguration.configd) to mirror network
    # state. A second configd_sim without plugins, registered under that name, serves it.
    hostcfg = {"Label": "com.x86code.host-configd",
               "ProgramArguments": ["/usr/libexec/configd_sim", "-b"],
               "EnvironmentVariables": {"SCD_SERVER": "com.apple.SystemConfiguration.configd"},
               "MachServices": {"com.apple.SystemConfiguration.configd": True}}
    if os.environ.get("X86CODE_JOBLOG") == "1":
        log = darling(data + "/var/log/com.x86code.host-configd.log")
        hostcfg.update({"StandardOutPath": log, "StandardErrorPath": log})
        hostcfg["EnvironmentVariables"]["OS_ACTIVITY_DT_MODE"] = "1"
    with open(os.path.join(daemons, "com.x86code.host-configd.plist"), "wb") as f:
        plistlib.dump(hostcfg, f)

    # x86Code: the host's network configuration in that store (what a Mac's configd publishes: Setup:/State: service
    # and interface keys of the primary interface), from Linux (linux/hostnet/hostnet.py), loaded by netconfig.
    # configd_sim's SimulatorSupport mirrors it into the simulator, whose IPMonitor derives State:/Network/Global/IPv4,
    # DNS and the nw_path/reachability state from it. X86CODE_NETCONFIG=0 leaves it out.
    if os.environ.get("X86CODE_NETCONFIG", "1") != "0" and host_path("host/netconfig/netconfig"):
        import subprocess
        hostnet = data + "/var/run/x86code-hostnet.plist"
        subprocess.run([sys.executable, PROJECT + "/linux/hostnet/hostnet.py", hostnet], check=False)
        depth = runtime_climb_depth()
        log = darling(data + "/var/log/com.x86code.netconfig.log")
        with open(os.path.join(daemons, "com.x86code.netconfig.plist"), "wb") as f:
            plistlib.dump({"Label": "com.x86code.netconfig",
                           "ProgramArguments": ["/.." * depth + darling(host_path("host/netconfig/netconfig")),
                                                darling(hostnet)],
                           "EnvironmentVariables": {"X86CODE_HOST_STORE": "1",
                                                    "OS_ACTIVITY_MODE": "disable"},
                           "RunAtLoad": True, "StandardOutPath": log, "StandardErrorPath": log}, f)

    # x86Code: the Metal stand-in (host/metalhostd): metalhostd serves the simulator's Metal transport on mtlvk
    # (Vulkan, a Linux process; cli/libexec/rundev.sh starts it and sets X86CODE_METAL=all unless told otherwise).
    # Here (mksimdevice alone) it is opt-in: X86CODE_METAL=1 registers
    # it for the test client (process "mtlprobe"), X86CODE_METAL=name,name for those processes,
    # X86CODE_METAL=all also under the generic com.apple.metal.simulator, which hands a device to every process
    # (CoreAnimation, the wallpaper poster, apps).
    metal = os.environ.get("X86CODE_METAL", "")
    if metal and metal != "0" and host_path("host/metalhostd/metalhostd"):
        procs = ["mtlprobe"] if metal == "1" else [p for p in metal.split(",") if p and p != "all"]
        names = ["com.apple.metal.simulator." + p for p in procs]
        if "all" in metal.split(","):
            names.append("com.apple.metal.simulator")
        log = darling(data + "/var/log/com.x86code.metalhostd.log")
        depth = runtime_climb_depth()
        menv = {"X86CODE_METAL_NAMES": ",".join(names), "OS_ACTIVITY_MODE": "disable",
                # x86Code: mtlvk's socket (Linux path; metalhostd prefixes /Volumes/SystemRoot)
                "X86CODE_MTLVK_SOCKET": MTLVK_SOCKET}
        for k in ("X86CODE_VKMTL_LOG", "X86CODE_VKMTL_DUMP", "X86CODE_METAL_PROFILE"):
            if os.environ.get(k):
                menv[k] = os.environ[k]
        with open(os.path.join(daemons, "com.x86code.metalhostd.plist"), "wb") as f:
            # RunAtLoad: under the A64 JIT, on-demand spawn+warmup of metalhostd is slow enough that
            # CoreAnimation can time out waiting for com.apple.metal.simulator.*; start it at boot.
            plistlib.dump({"Label": "com.x86code.metalhostd",
                           "ProgramArguments": ["/.." * depth + darling(host_path("host/metalhostd/metalhostd"))],
                           "MachServices": {n: True for n in names}, "EnvironmentVariables": menv,
                           "RunAtLoad": True, "KeepAlive": True,
                           "StandardOutPath": log, "StandardErrorPath": log}, f)
        # x86Code: iOS 27 MTLCompiler lives on disk as Metal.framework/XPCServices/MTLCompilerService.xpc
        # (moved out of the dyld cache). launchd_sim discovers it as a multi-instance Application XPC
        # service (same as iOS 26.5 on x86). Do NOT install a single-instance LaunchDaemon for the
        # same label: that makes "create copy of service" fail with error 137 and every pipeline dies.
        # A64: Metal.framework's executable is cache-only; without an on-disk Metal binary, xpc_bundle
        # cannot resolve the framework and MTLCompiler lookups are "No such process" (error 3).
        # cli/libexec/arm64/ensure-metal-framework.sh extracts Metal from the dyld cache into the
        # framework (runtimes/ is gitignored; never commit the binary).

    # x86Code: cross-process IOSurfaces (host/iosurfaced): iosurfaced serves com.apple.IOSurface.Remote (what
    # CoreSimulator's SimRenderServer does on a Mac) and IOSURFACE_REMOTE_EMULATED is not set, so every process's
    # IOSurface.framework uses it. X86CODE_IOSURFACE=0 goes back to per-process emulation.
    iosurface = os.environ.get("X86CODE_IOSURFACE", "1") not in ("", "0") and bool(host_path("host/iosurfaced/iosurfaced"))
    if iosurface:
        log = darling(data + "/var/log/com.x86code.iosurfaced.log")
        depth = runtime_climb_depth()
        ienv = {"OS_ACTIVITY_MODE": "disable"}
        if os.environ.get("X86CODE_IOSURFACE_LOG"):
            ienv["X86CODE_IOSURFACE_LOG"] = "1"
        with open(os.path.join(daemons, "com.x86code.iosurfaced.plist"), "wb") as f:
            plistlib.dump({"Label": "com.x86code.iosurfaced",
                           "ProgramArguments": ["/.." * depth + darling(host_path("host/iosurfaced/iosurfaced"))],
                           "MachServices": {"com.apple.IOSurface.Remote": True}, "EnvironmentVariables": ienv,
                           "KeepAlive": True, "StandardOutPath": log, "StandardErrorPath": log}, f)

    if os.environ.get("X86CODE_BOOTSTRAP_SYSTEM") == "1":
        # X86CODE_BOOTSTRAP_SYSTEM=1: experimental job asking launchd_sim to bootstrap the
        # system domain (on a Mac it leaves bootstrap mode; here it never does)
        log = darling(data + "/var/log/com.x86code.bootstrap-system.log")
        with open(os.path.join(daemons, "com.x86code.bootstrap-system.plist"), "wb") as f:
            plistlib.dump({"Label": "com.x86code.bootstrap-system",
                           "ProgramArguments": ["/bin/launchctl", "bootstrap", "system", darling(daemons)],
                           "RunAtLoad": True, "StandardOutPath": log, "StandardErrorPath": log}, f)
    # Darling's libkqueue for processes on the runtime's libSystem (see docs/notebook/FINDINGS.md)
    kqueue_sim = darling(PREFIX + "/libexec/darling/usr/lib/darling/libkqueue_sim.dylib")
    child_defaults = {
        # X86CODE_INSERT: extra ':'-separated dylibs (host paths) for diagnostics
        # simshim: stand-ins for host services (host/simshim/compat/simshim.c)
        # simaudio: the audio devices (host/simaudio, opt-in: X86CODE_AUDIO=1)
        "DYLD_INSERT_LIBRARIES": ":".join(([] if ARM64 else [kqueue_sim]) +
                                          [darling(p) for p in [host_path("host/simshim/compat/libsimshim.dylib")] if p] +
                                          [darling(p) for p in [host_path("host/simaudio/libsimaudio.dylib")] if p and AUDIO] + [darling(os.path.abspath(p))
                                          for p in os.environ.get("X86CODE_INSERT", "").split(":") if p]),
        "CFFIXED_USER_HOME": home,
        "HOME": home,
        "TMPDIR": home + "/tmp/",
        "DYLD_FALLBACK_FRAMEWORK_PATH": rt + "/System/Library/Frameworks",
        "DYLD_FALLBACK_LIBRARY_PATH": rt + "/usr/lib",
    }
    # the x86_64 dyld shared cache (build/mac-build-dyldcache.sh; FINDINGS, dyld shared cache): dyld_sim maps it at 0x7FF800000000,
    # which Darling's mldr reserves for processes that have this variable. X86SIM_DYLD_CACHE=0: no cache
    cache = os.path.join(PROJECT, "runtimes", "dyld-cache-23F73-x86_64")
    if not ARM64 and os.environ.get("X86SIM_DYLD_CACHE", "1") != "0" and os.path.isfile(cache + "/dyld_sim_shared_cache_x86_64"):
        child_defaults["DYLD_SHARED_CACHE_DIR"] = darling(cache) + "/"
    env = dict(child_defaults)
    env.update({
        "DYLD_ROOT_PATH": rt,
        "IPHONE_SIMULATOR_ROOT": rt,
        "SIMULATOR_ROOT": rt,
        "IPHONE_SHARED_RESOURCES_DIRECTORY": home,
        "SIMULATOR_SHARED_RESOURCES_DIRECTORY": home,
        "PATH": ":".join(rt + p for p in ["/usr/bin", "/bin", "/usr/sbin", "/sbin"]),
        "SIMULATOR_ARCHS": DEVTYPE["arch"],
        # x86Code: libarm64jit's log (guest crashes, unimplemented thunks) per process, inside the container
        **({"X86CODE_A64_LOG": "/private/var/tmp/x86code-a64"} if ARM64 else {}),
        # x86Code: glibc malloc of libarm64jit/dynarmic (the ELF side): one arena per thread kept tens of MB of freed
        # JIT metadata resident per thread; two arenas for the process
        **({"GLIBC_TUNABLES": "glibc.malloc.arena_max=2:glibc.malloc.trim_threshold=1048576"} if ARM64 else {}),
        # x86Code: its own frame ring, so an x86_64 device's viewers are not disturbed
        # Speed 8: in /run/user/<uid> (the arm64 container's /dev/shm is private, X86CODE_PRIVATE_SHM); boot-a64.sh links
        # /dev/shm/x86code-a64-simfb.ring to it for the host's viewers
        **({"SIMFB_RING": os.environ.get("SIMFB_RING", f"/Volumes/SystemRoot/run/user/{os.getuid()}/x86code-a64-simfb.ring")} if ARM64 else {}),
        # Speed 3: launchd jobs only see Bootstrap.Child, so the JIT flags must be copied here to reach backboardd and
        # SpringBoard. Speed 6: every X86CODE_A64_* flag (was a list that missed new ones, e.g. PERFMAP).
        **({k: v for k, v in os.environ.items()
            if (k.startswith("X86CODE_A64_") and k not in ("X86CODE_A64_EXE", "X86CODE_A64_LOG")) or k == "SIMFB_HZ"}
           if ARM64 else {}),
        "SIMULATOR_UDID": udid,
        "SIMULATOR_DEVICE_NAME": dt.get("name", "x86code"),
        "SIMULATOR_RUNTIME_VERSION": dt["version"],
        "SIMULATOR_RUNTIME_BUILD_VERSION": dt["build"],
        "SIMULATOR_MODEL_IDENTIFIER": dt["model"],
        "SIMULATOR_PRODUCT_CLASS": dt["class"],
        "SIMULATOR_MAINSCREEN_WIDTH": dt["w"],
        "SIMULATOR_MAINSCREEN_HEIGHT": dt["h"],
        "SIMULATOR_MAINSCREEN_SCALE": dt["scale"],
        "SIMULATOR_MAINSCREEN_PITCH": dt["pitch"],
        "SIMULATOR_LEGACY_ASSET_SUFFIX": dt["suffix"],
        # x86Code: the device type's capabilities (CoreSimulator: <device type>.simdevicetype/Contents/Resources/
        # capabilities.plist), which libMobileGestalt answers from (ArtworkTraits, displays, DeviceCornerRadius, ...).
        # Without it the runtime's apps got by, but an installed app aborted in UIKit's
        # -[_UIScreenComplexBoundingPathUtilities _loadBitmapForScreen:type:] (no kMGArtworkDeviceIdiom).
        "SIMULATOR_CAPABILITIES": darling(capabilities),
        "SIMULATOR_HOST_HOME": home,
        "SIMULATOR_LOG_ROOT": home + "/Library/Logs",
        # x86Code: the simulator's libdispatch implements DISPATCH_SOURCE_TYPE_MEMORYPRESSURE as an EVFILT_VNODE watch
        # (NOTE_ATTRIB) on this file (CoreSimulator touches it for "Simulate Memory Warning"); without it the source
        # is registered on fd -1 and libdispatch crashes the process (fileproviderd in LaunchServices' memory warning
        # listener). A Mac's device has the same empty file and variable.
        "SIMULATOR_MEMORY_WARNINGS": home + "/var/run/memory_warning_simulation",
        "SIMULATOR_BOOT_TIME": str(int(os.path.getmtime(data))),
        "SIMULATOR_VERSION_INFO": "x86Code - Device: x86code (%s) - Runtime: iOS %s (%s)" % (udid, dt["version"], dt["build"]),
        "XPC_SIMULATOR_LAUNCHD_NAME": "com.apple.CoreSimulator.SimDevice." + udid,
        # x86Code: no XPC_SERVICE_NAME here. launchd_sim gives each job XPC_SERVICE_NAME=<label> only when its own
        # environment has none, so every job inherited the device's name: lsd's [NSProcessInfo bs_jobLabel] wasn't
        # com.apple.lsd and FrontBoardServices' "LaunchServices-only" assertion aborted lsd on every openURL.
        "LS_ENABLE_BUNDLE_LOCALIZATION_CACHING": "1",
        # x86Code: no CoreSimulator host; IOSurface allocates in-process, our SimFramebuffer fakes the display
        **({} if iosurface else {"IOSURFACE_REMOTE_EMULATED": "1"}),
        # x86Code: metalhostd is in-sim (not Mac SimMetalHost). simshim skips sim2host framing for
        # com.apple.metal.simulator* and, when this is set, for enable_sim2host calls before set_name (iOS 27).
        **({"X86CODE_METAL_IN_SIM": "1"} if metal else {}),
        **({"X86CODE_SIMSHIM_XPC_LOG": "1"} if os.environ.get("X86CODE_SIMSHIM_XPC_LOG") else {}),
        # X86CODE_SHIMLOG=1: simshim logs what it intercepts
        **({"X86CODE_SHIMLOG": "1"} if os.environ.get("X86CODE_SHIMLOG") else {}),
        **({"X86CODE_AUDIO_SOCKET": darling(AUDIO_SOCKET)} if AUDIO else {}),
        **({"X86CODE_AUDIO_LOG": "1"} if os.environ.get("X86CODE_AUDIO_LOG") else {}),
        **({"SIMULATOR_FRAMEBUFFER_FRAMEWORK": darling(host_path("host/simfb/SimFramebuffer.framework/SimFramebuffer"))}
           if host_path("host/simfb/SimFramebuffer.framework/SimFramebuffer") else {}),
        # x86Code: our SimulatorHID stand-in (host/simhid): backboardd's virtual touchscreen/buttons/keyboard,
        # fed through <data>/simhid.sock by cli/libexec/simview.py / cli/libexec/simtouch.py. X86CODE_HID=0 leaves it out.
        **({} if os.environ.get("X86CODE_HID") == "0" or not host_path("host/simhid/SimulatorHID.framework") else
           {"SIMULATOR_HID_SYSTEM_MANAGER": darling(host_path("host/simhid/SimulatorHID.framework"))}),
    })

    bootstrap = {
        "AbortOnUnknownIPC": True,
        "CrashOnSIGTERMTimeout": False,
        "ExtensionWatchdog": 0,
        "LogServiceLifecycle": True,
        "LogToFlatFile": True,
        "RequireDomainBinding": True,
        "SIGTERMTimeout": 3,
        "UseGuardMalloc": False,
        "Bootstrap": {
            "BinaryOrderPreference": DEVTYPE["arch"],
            "CheckedAllocationsPosixSpawnFlagsGlobal": False,
            # As CoreSimulator's: HOME, CFFIXED_USER_HOME and TMPDIR are only defaults, so an
            # app's own values (its data container, set by RunningBoard) win; "Child" would force them
            "Child": {k: v for k, v in env.items() if k not in ("HOME", "CFFIXED_USER_HOME", "TMPDIR")},
            "ChildDefaults": child_defaults,
            "Self": env,
            "Paths": [darling(daemons), darling(angels)],
            "SimulatorRuntimeRoot": darling(os.path.realpath(RUNTIME)) if ARM64 else rt,
        },
    }
    with open(data + "/var/run/launchd_bootstrap.plist", "wb") as f:
        plistlib.dump(bootstrap, f)
    with open(device + "/sim.env", "w") as f:
        for k, v in env.items():
            f.write("%s=%s\n" % (k, v))
    print(device, udid)


if __name__ == "__main__":
    main()
