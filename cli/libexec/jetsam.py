#!/usr/bin/env python3
"""x86Code: a jetsam stand-in. Reclaims processes RunningBoard has suspended, as a device's jetsam does.

RunningBoard suspends idle extensions and background apps (pid_suspend; Darling records each suspension as
/private/var/run/x86code-suspended/<pid> in the container, removed on pid_resume). On a device, memorystatus kills
suspended processes in the idle bands when memory runs short. The simulator has no jetsam, and here every process
carries ~200 MB of private framework data (no dyld shared cache), so after a boot ~40 widget, poster and file-provider
extensions that chronod and PosterBoard started stayed resident (~6 GB with swap). This kills (SIGKILL, like jetsam):
  - an app extension (.appex) suspended for more than X86SIM_JETSAM_EXT seconds (default 10)
  - a background app (.app) suspended for more than X86SIM_JETSAM_APP seconds (default 30), except the
    X86SIM_JETSAM_KEEP most recently suspended ones (default 1)
Daemons are never touched; RunningBoard relaunches what it needs, as after a jetsam kill. Something relaunched right
away (the wallpaper's poster extension) would be killed and relaunched in a loop: each kill of the same executable
doubles its grace period (up to 10 minutes), and one that comes back within RELAUNCH s of its kill is never killed
again (SpringBoard relaunches Spotlight, its home-screen search host, on the next home-button press: each kill cost a
full cold launch, ~1.5 s of CPU, usually right when the user opens an app). X86SIM_JETSAM=0 disables it.
    cli/libexec/jetsam.py [--once] [-v]"""
import os, signal, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "lib"))
import x86config  # noqa: E402

DIR = os.path.join(x86config.get("X86SIM_DPREFIX"), "private/var/run/x86code-suspended")
EXT = float(os.environ.get("X86SIM_JETSAM_EXT", "10"))
APP = float(os.environ.get("X86SIM_JETSAM_APP", "30"))
KEEP = int(os.environ.get("X86SIM_JETSAM_KEEP", "1"))
POLL = 2.0
CLK = os.sysconf("SC_CLK_TCK")
BOOT = time.time() - float(open("/proc/uptime").read().split()[0])
BACKOFF_MAX = 600.0
_kills = {}   # executable -> kills so far (each doubles its grace period)
_killed_at = {}   # executable -> time of its last kill
_kept = set()   # executables relaunched right after a kill: left alone from then on
RELAUNCH = 60.0
NEVER = ("SpringBoard.app/", "PosterBoard.app/", "Spotlight.app/")   # system apps RunningBoard may background; never reclaimed here


def darling_pids():
    """Darling pid (the container namespace's) -> (Linux pid, executable path, start time as epoch seconds)."""
    out = {}
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            with open("/proc/%s/status" % p) as f:
                ns = next((l.split()[1:] for l in f if l.startswith("NSpid:")), [])
            if len(ns) < 2:
                continue
            with open("/proc/%s/cmdline" % p, "rb") as f:
                exe = f.read().split(b"\0")[0].decode(errors="replace")
            with open("/proc/%s/stat" % p) as f:
                start = BOOT + int(f.read().rsplit(")", 1)[1].split()[19]) / CLK
            out[int(ns[-1])] = (int(p), exe, start)
        except (OSError, ValueError, IndexError):
            pass
    return out


def sweep(verbose=False):
    try:
        names = os.listdir(DIR)
    except OSError:
        return []
    now = time.time()
    marks = []
    for n in names:
        if not n.isdigit():
            continue
        try:
            marks.append((int(n), os.stat(os.path.join(DIR, n)).st_mtime))
        except OSError:
            pass
    if not marks:
        return []
    procs = darling_pids()
    for _, exe, start in procs.values():
        k = _killed_at.get(exe)
        if k is not None and exe not in _kept and k < start < k + RELAUNCH:
            _kept.add(exe)
            print("%s jetsam: %s came back %.0f s after its kill; keeping it" % (
                time.strftime("%H:%M:%S"), exe.split("/")[-1], start - k), flush=True)
    apps = []
    killed = []
    for dpid, since in marks:
        pr = procs.get(dpid)
        if not pr or pr[2] > since + 1:   # gone, or the pid was reused after the suspension was recorded
            continue
        lpid, exe, _ = pr
        if exe in _kept:
            continue
        age = now - since
        grace = min(BACKOFF_MAX, (EXT if ".appex/" in exe else APP) * (2 ** _kills.get(exe, 0)))
        if ".appex/" in exe:
            if age > grace:
                killed.append((lpid, dpid, exe, age))
        elif ".app/" in exe and not any(k in exe for k in NEVER):
            apps.append((since, lpid, dpid, exe, age))
    apps.sort(reverse=True)   # most recently suspended first
    for since, lpid, dpid, exe, age in apps[KEEP:]:
        if age > min(BACKOFF_MAX, APP * (2 ** _kills.get(exe, 0))):
            killed.append((lpid, dpid, exe, age))
    for lpid, dpid, exe, age in killed:
        try:
            os.kill(lpid, signal.SIGKILL)
            _kills[exe] = _kills.get(exe, 0) + 1
            _killed_at[exe] = time.time()
            print("%s jetsam: killed %s (pid %d, suspended %.0f s)" % (
                time.strftime("%H:%M:%S"), exe.split("/")[-1], dpid, age), flush=True)
        except OSError:
            pass
    if verbose:
        print("%d suspended, %d killed" % (len(marks), len(killed)), flush=True)
    return killed


def main(a):
    if os.environ.get("X86SIM_JETSAM", "1") == "0":
        return
    if "--once" in a:
        sweep("-v" in a)
        return
    while True:
        sweep("-v" in a)
        time.sleep(POLL)


if __name__ == "__main__":
    main(sys.argv[1:])
