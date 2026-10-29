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
PREFIX = x86config.get("X86SIM_PREFIX")
MTLVK_SOCKET = os.environ.get("X86CODE_MTLVK_SOCKET") or os.path.join(x86config.get("X86SIM_STATE"), "mtlvk.sock")
# x86Code: the host side of the simulator's audio (cli/libexec/simaudio.py, started by rundev.sh)
AUDIO_SOCKET = os.environ.get("X86CODE_AUDIO_SOCKET") or os.path.join(x86config.get("X86SIM_STATE"), "simaudio.sock")
AUDIO = os.environ.get("X86CODE_AUDIO", "0") != "0"  # opt-in until playback works (FINDINGS 3.18)
DARLING_VIEW = "/Volumes/SystemRoot"  # Linux / as seen inside Darling


def darling(path):
    return DARLING_VIEW + path


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
    # in their own directory; CoreSimulator hands launchd_sim that path too
    angels = device + "/LaunchAngels"
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
            depth = len(darling(RUNTIME).strip("/").split("/"))
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
        with open(os.path.join(dst, job + ".plist"), "wb") as f:
            plistlib.dump(plist, f)

    rt, home = darling(RUNTIME), darling(data)
    # x86Code: iPhone 16 (iPhone17,3) device type capabilities, as JSON in cli/lib, written as a plist for MobileGestalt
    capabilities = device + "/capabilities.plist"
    with open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "lib", "devicetype-iphone17,3.json")) as f:
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
    if os.environ.get("X86CODE_NETCONFIG", "1") != "0":
        import subprocess
        hostnet = data + "/var/run/x86code-hostnet.plist"
        subprocess.run([sys.executable, PROJECT + "/linux/hostnet/hostnet.py", hostnet], check=False)
        depth = len(darling(RUNTIME).strip("/").split("/"))
        log = darling(data + "/var/log/com.x86code.netconfig.log")
        with open(os.path.join(daemons, "com.x86code.netconfig.plist"), "wb") as f:
            plistlib.dump({"Label": "com.x86code.netconfig",
                           "ProgramArguments": ["/.." * depth + darling(PROJECT + "/host/netconfig/netconfig"),
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
    if metal and metal != "0":
        procs = ["mtlprobe"] if metal == "1" else [p for p in metal.split(",") if p and p != "all"]
        names = ["com.apple.metal.simulator." + p for p in procs]
        if "all" in metal.split(","):
            names.append("com.apple.metal.simulator")
        log = darling(data + "/var/log/com.x86code.metalhostd.log")
        depth = len(darling(RUNTIME).strip("/").split("/"))
        menv = {"X86CODE_METAL_NAMES": ",".join(names), "OS_ACTIVITY_MODE": "disable",
                # x86Code: mtlvk's socket (Linux path; metalhostd prefixes /Volumes/SystemRoot)
                "X86CODE_MTLVK_SOCKET": MTLVK_SOCKET}
        for k in ("X86CODE_VKMTL_LOG", "X86CODE_VKMTL_DUMP", "X86CODE_METAL_PROFILE"):
            if os.environ.get(k):
                menv[k] = os.environ[k]
        with open(os.path.join(daemons, "com.x86code.metalhostd.plist"), "wb") as f:
            plistlib.dump({"Label": "com.x86code.metalhostd",
                           "ProgramArguments": ["/.." * depth + darling(PROJECT + "/host/metalhostd/metalhostd")],
                           "MachServices": {n: True for n in names}, "EnvironmentVariables": menv,
                           "StandardOutPath": log, "StandardErrorPath": log}, f)

    # x86Code: cross-process IOSurfaces (host/iosurfaced): iosurfaced serves com.apple.IOSurface.Remote (what
    # CoreSimulator's SimRenderServer does on a Mac) and IOSURFACE_REMOTE_EMULATED is not set, so every process's
    # IOSurface.framework uses it. X86CODE_IOSURFACE=0 goes back to per-process emulation.
    iosurface = os.environ.get("X86CODE_IOSURFACE", "1") not in ("", "0")
    if iosurface:
        log = darling(data + "/var/log/com.x86code.iosurfaced.log")
        depth = len(darling(RUNTIME).strip("/").split("/"))
        ienv = {"OS_ACTIVITY_MODE": "disable"}
        if os.environ.get("X86CODE_IOSURFACE_LOG"):
            ienv["X86CODE_IOSURFACE_LOG"] = "1"
        with open(os.path.join(daemons, "com.x86code.iosurfaced.plist"), "wb") as f:
            plistlib.dump({"Label": "com.x86code.iosurfaced",
                           "ProgramArguments": ["/.." * depth + darling(PROJECT + "/host/iosurfaced/iosurfaced")],
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
        # simaudio: the audio devices (host/simaudio, X86CODE_AUDIO=0 leaves it out)
        "DYLD_INSERT_LIBRARIES": ":".join([kqueue_sim, darling(PROJECT + "/host/simshim/compat/libsimshim.dylib")] +
                                          ([darling(PROJECT + "/host/simaudio/libsimaudio.dylib")] if AUDIO else []) + [darling(os.path.abspath(p))
                                          for p in os.environ.get("X86CODE_INSERT", "").split(":") if p]),
        "CFFIXED_USER_HOME": home,
        "HOME": home,
        "TMPDIR": home + "/tmp/",
        "DYLD_FALLBACK_FRAMEWORK_PATH": rt + "/System/Library/Frameworks",
        "DYLD_FALLBACK_LIBRARY_PATH": rt + "/usr/lib",
    }
    env = dict(child_defaults)
    env.update({
        "DYLD_ROOT_PATH": rt,
        "IPHONE_SIMULATOR_ROOT": rt,
        "SIMULATOR_ROOT": rt,
        "IPHONE_SHARED_RESOURCES_DIRECTORY": home,
        "SIMULATOR_SHARED_RESOURCES_DIRECTORY": home,
        "PATH": ":".join(rt + p for p in ["/usr/bin", "/bin", "/usr/sbin", "/sbin"]),
        "SIMULATOR_ARCHS": "x86_64",
        "SIMULATOR_UDID": udid,
        "SIMULATOR_DEVICE_NAME": "x86code",
        "SIMULATOR_RUNTIME_VERSION": "26.5",
        "SIMULATOR_RUNTIME_BUILD_VERSION": "23F73",
        "SIMULATOR_MODEL_IDENTIFIER": "iPhone17,3",
        "SIMULATOR_PRODUCT_CLASS": "D47",
        "SIMULATOR_MAINSCREEN_WIDTH": "1179",
        "SIMULATOR_MAINSCREEN_HEIGHT": "2556",
        "SIMULATOR_MAINSCREEN_SCALE": "3.000000",
        "SIMULATOR_MAINSCREEN_PITCH": "460.000000",
        "SIMULATOR_LEGACY_ASSET_SUFFIX": "iphone",
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
        "SIMULATOR_VERSION_INFO": "x86Code - Device: x86code (%s) - Runtime: iOS 26.5 (23F73)" % udid,
        "XPC_SIMULATOR_LAUNCHD_NAME": "com.apple.CoreSimulator.SimDevice." + udid,
        # x86Code: no XPC_SERVICE_NAME here. launchd_sim gives each job XPC_SERVICE_NAME=<label> only when its own
        # environment has none, so every job inherited the device's name: lsd's [NSProcessInfo bs_jobLabel] wasn't
        # com.apple.lsd and FrontBoardServices' "LaunchServices-only" assertion aborted lsd on every openURL.
        "LS_ENABLE_BUNDLE_LOCALIZATION_CACHING": "1",
        # x86Code: no CoreSimulator host; IOSurface allocates in-process, our SimFramebuffer fakes the display
        **({} if iosurface else {"IOSURFACE_REMOTE_EMULATED": "1"}),
        # X86CODE_SHIMLOG=1: simshim logs what it intercepts
        **({"X86CODE_SHIMLOG": "1"} if os.environ.get("X86CODE_SHIMLOG") else {}),
        **({"X86CODE_AUDIO_SOCKET": darling(AUDIO_SOCKET)} if AUDIO else {}),
        **({"X86CODE_AUDIO_LOG": "1"} if os.environ.get("X86CODE_AUDIO_LOG") else {}),
        "SIMULATOR_FRAMEBUFFER_FRAMEWORK": DARLING_VIEW + PROJECT + "/host/simfb/SimFramebuffer.framework/SimFramebuffer",
        # x86Code: our SimulatorHID stand-in (host/simhid): backboardd's virtual touchscreen/buttons/keyboard,
        # fed through <data>/simhid.sock by cli/libexec/simview.py / cli/libexec/simtouch.py. X86CODE_HID=0 leaves it out.
        **({} if os.environ.get("X86CODE_HID") == "0" else
           {"SIMULATOR_HID_SYSTEM_MANAGER": DARLING_VIEW + PROJECT + "/host/simhid/SimulatorHID.framework"}),
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
            "BinaryOrderPreference": "x86_64",
            "CheckedAllocationsPosixSpawnFlagsGlobal": False,
            # As CoreSimulator's: HOME, CFFIXED_USER_HOME and TMPDIR are only defaults, so an
            # app's own values (its data container, set by RunningBoard) win; "Child" would force them
            "Child": {k: v for k, v in env.items() if k not in ("HOME", "CFFIXED_USER_HOME", "TMPDIR")},
            "ChildDefaults": child_defaults,
            "Self": env,
            "Paths": [darling(daemons), darling(angels)],
            "SimulatorRuntimeRoot": rt,
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
