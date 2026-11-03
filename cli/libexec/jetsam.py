#!/usr/bin/env python3
"""x86Code: a jetsam stand-in. Reclaims what a device's memorystatus would: idle extensions, background apps and clean
idle-exit daemons.

On a device memorystatus keeps every process in a band and, when memory runs short, kills the idle band first (clean
daemons launchd lets exit, suspended apps and extensions), then background apps. The simulator has no jetsam, and here
every process carries ~100-500 MB of private memory (JIT, framework data), so nothing was ever reclaimed: the widget,
poster and file-provider extensions chronod and PosterBoard started (~6 GB at a boot), each app opened, and every daemon
a boot's launch events started (Siri's, Wallet's, Maps': ~1 GB). The phone runs as if memory were always short (a host
runs several phones), so this reclaims as soon as something has been idle a while. What it reads (Darling's
libsystem_kernel records it in the container, cleared at each boot):
  /private/var/run/x86code-suspended/<pid>   RunningBoard suspended it (pid_suspend; removed on pid_resume)
  /private/var/run/x86code-jetsam/<pid>      "<band> <dirty flags> <user data> <darwin bg>" (process/memorystatus.c):
      dirty flags as proc_get_dirty: 1 tracked, 2 allows idle exit (launchd: EnablePressuredExit), 4 dirty (an open
      XPC transaction); darwin bg 1 = RunningBoard backgrounded it (PRIO_DARWIN_BG), 0 = brought back; darwin role
      (PRIO_DARWIN_ROLE) 3 = off screen (NON_UI), 6 = DARWIN_BG, 1/2/4 = on screen.
Kills (SIGKILL, as jetsam; launchd / RunningBoard relaunch what is needed on the next lookup or open):
  - an app extension (.appex) suspended for more than X86SIM_JETSAM_EXT s (default 10)
  - a background app (.app: suspended, backgrounded, or off screen: Maps, which keeps a background mode, is never
    suspended or PRIO_DARWIN_BG'd; its role goes to NON_UI when the user leaves it) after
    X86SIM_JETSAM_APP s (default 30); the most recently backgrounded one after X86SIM_JETSAM_LAST s (default 90),
    so switching back to the last app soon is a warm resume. X86SIM_JETSAM_KEEP (default 1): how many get the longer time.
  - an idle-exit daemon (tracked, allows idle exit, clean) that has been clean and used no CPU for X86SIM_JETSAM_IDLE s
    (default 30); the daemons every app launch talks to (HOT, X86SIM_JETSAM_HOT) after X86SIM_JETSAM_IDLE_HOT s
    (default 600), so app opens don't pay their relaunch. While an app is in the foreground every daemon gets the HOT
    time (the app's own daemons, e.g. geod under Maps, stay warm).
  - X86SIM_JETSAM_DEMAND (default below): on-demand daemons without EnablePressuredExit that a basic phone only needs
    for a feature in use (Siri's assets, Wallet's passd): as idle-exit daemons once clean (tracked, not dirty).
Something relaunched within RELAUNCH s of its kill (a launch event, or a client that needed it) doubles its grace
period each time, up to X86SIM_JETSAM_MAX s (default 1800): it is never kept forever, and never killed in a loop.
X86SIM_JETSAM=0 disables it; X86SIM_JETSAM_DAEMONS=0 leaves daemons alone (extensions and apps only).
    cli/libexec/jetsam.py [--once] [-v] [--status]"""
import os, signal, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "lib"))
import x86config  # noqa: E402

PREFIX = x86config.get("X86SIM_DPREFIX")
DIR = os.path.join(PREFIX, "private/var/run/x86code-suspended")
JDIR = os.path.join(PREFIX, "private/var/run/x86code-jetsam")
EXT = float(os.environ.get("X86SIM_JETSAM_EXT", "10"))
APP = float(os.environ.get("X86SIM_JETSAM_APP", "30"))
LAST = float(os.environ.get("X86SIM_JETSAM_LAST", "90"))
KEEP = int(os.environ.get("X86SIM_JETSAM_KEEP", "1"))
IDLE = float(os.environ.get("X86SIM_JETSAM_IDLE", "30"))
IDLE_HOT = float(os.environ.get("X86SIM_JETSAM_IDLE_HOT", "600"))
DAEMONS = os.environ.get("X86SIM_JETSAM_DAEMONS", "1") != "0"
BACKOFF_MAX = float(os.environ.get("X86SIM_JETSAM_MAX", "1800"))
# every app launch / the home screen talks to these (containers, code signing, TCC, icons, fonts, extensions, status bar)
HOT = set(os.environ.get("X86SIM_JETSAM_HOT", "containermanagerd,containermanagerd_system,trustd,securityd,"
                         "iconservicesagent,fontservicesd,pkd,appprotectiond,tccd,systemstatusd,usernotificationsd,"
                         "sleepd,diagnosticd,mobilegestalt,UserEventAgent,nsurlsessiond,cloudd").split(","))
DEMAND = set(os.environ.get("X86SIM_JETSAM_DEMAND", "passd,mobileassetd,assistant_cdmd,ContextService,"
                            "STExtractionService,localspeechrecognition,com.apple.datamigrator,"
                            "com.apple.MobileAsset.DownloadService.Builtin,UserFontManager,siriactionsd,"
                            "assetsubscriptiond,AppIntentsLiveEntityService").split(",")) - {""}
POLL = 2.0
# RAM 2: an app RunningBoard never backgrounds (Maps keeps a background mode) is asked about: com.x86code.openurl writes
# SpringBoard's frontmost app on request (host/openurl). Asked only while such an app has been "foreground" past APP s.
DEVDATA = os.path.join(os.environ.get("X86SIM_DEVICE", ""), "data")
FRONT_REQ = os.path.join(DEVDATA, "tmp/x86code-frontmost.req")
FRONT = os.path.join(DEVDATA, "tmp/x86code-frontmost")
FRONT_EVERY = 10.0
_front = {"asked": 0.0}
_bundle_ids = {}
CPU_IDLE = 0.2   # CPU seconds a process may use per CPU_WIN and still count as idle
CPU_WIN = 10.0
CLK = os.sysconf("SC_CLK_TCK")
BOOT = time.time() - float(open("/proc/uptime").read().split()[0])
RELAUNCH = 60.0
# system apps and UI services (angels: the keyboard, Siri's UI) RunningBoard may background: never reclaimed here, and
# never "the app in the foreground"
NEVER = ("SpringBoard.app/", "PosterBoard.app/", "Spotlight.app/", "InputUI.app/", "Campo.app/")
_kills = {}       # executable -> quick relaunches so far (each doubles its grace period)
_killed_at = {}   # executable -> time of its last kill
_seen = {}        # (dpid, start) -> {"bg": since, "clean": since, "cpu": [(t, cpu s)]}


def container_ns():
    """The pid namespace of X86SIM_DPREFIX's container (its darlingserver's children), or None. Several containers can
    run at once (the arm64 device next to the x86_64 one, or a second arm64 instance): their Darling pids overlap."""
    prefix = os.path.realpath(PREFIX)
    servers = set()
    kids = {}
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % p) as f:
                kids.setdefault(int(f.read().rsplit(")", 1)[1].split()[1]), []).append(p)
            with open("/proc/%s/comm" % p) as f:
                if f.read().strip() != "darlingserver":
                    continue
            with open("/proc/%s/cmdline" % p, "rb") as f:
                args = f.read().split(b"\0")
            if len(args) > 1 and os.path.realpath(args[1].decode(errors="replace")) == prefix:
                servers.add(int(p))
        except (OSError, ValueError, IndexError):
            pass
    # the namespace most of the server's children are in (one child can be alone in its own: the boot's init helper;
    # taking the first child found made this see 1 process and reclaim nothing)
    count = {}
    for s in servers:
        for k in kids.get(s, []):
            try:
                ns = os.readlink("/proc/%s/ns/pid" % k)
            except OSError:
                continue
            count[ns] = count.get(ns, 0) + 1
    return max(count, key=count.get) if count else None


def darling_pids():
    """Darling pid (the container namespace's) -> (Linux pid, executable path, start time as epoch seconds, CPU s)."""
    out = {}
    ns_want = container_ns()
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            if ns_want and os.readlink("/proc/%s/ns/pid" % p) != ns_want:
                continue
            with open("/proc/%s/status" % p) as f:
                ns = next((l.split()[1:] for l in f if l.startswith("NSpid:")), [])
            if len(ns) < 2:
                continue
            with open("/proc/%s/cmdline" % p, "rb") as f:
                exe = f.read().split(b"\0")[0].decode(errors="replace")
            with open("/proc/%s/stat" % p) as f:
                st = f.read().rsplit(")", 1)[1].split()
            start = BOOT + int(st[19]) / CLK
            out[int(ns[-1])] = (int(p), exe, start, (int(st[11]) + int(st[12])) / CLK)
        except (OSError, ValueError, IndexError):
            pass
    return out


def marks(d):
    """pid -> (mtime, fields) of the files in d."""
    out = {}
    try:
        names = os.listdir(d)
    except OSError:
        return out
    for n in names:
        if not n.isdigit():
            continue
        path = os.path.join(d, n)
        try:
            mt = os.stat(path).st_mtime
            fields = open(path).read().split() if d == JDIR else []
            out[int(n)] = (mt, [int(x) for x in fields])
        except (OSError, ValueError):
            pass
    return out


def name_of(exe):
    return exe.rstrip("/").split("/")[-1]


def bundle_id(exe):
    """The CFBundleIdentifier of an app executable (its Info.plist, read on the host)."""
    if exe not in _bundle_ids:
        import plistlib
        path = exe[len("/Volumes/SystemRoot"):] if exe.startswith("/Volumes/SystemRoot/") else exe
        try:
            with open(os.path.join(os.path.dirname(path), "Info.plist"), "rb") as f:
                _bundle_ids[exe] = plistlib.load(f).get("CFBundleIdentifier")
        except Exception:
            _bundle_ids[exe] = None
    return _bundle_ids[exe]


def frontmost(now):
    """SpringBoard's frontmost app id ("" at home), None when unknown. Asks openurl at most every FRONT_EVERY s."""
    try:
        st = os.stat(FRONT)
        val = open(FRONT).read().strip()
    except OSError:
        st, val = None, None
    if now - _front["asked"] > FRONT_EVERY:
        _front["asked"] = now
        try:
            open(FRONT_REQ, "w").close()
        except OSError:
            pass
    if st is None or val == "?" or now - st.st_mtime > 3 * FRONT_EVERY:
        return None
    return val


def grace(exe, base):
    return min(BACKOFF_MAX, base * (2 ** _kills.get(exe, 0)))


def cpu_idle(st, now, cpu):
    """Records a CPU sample; True when the process used under CPU_IDLE s over the last CPU_WIN s."""
    h = st.setdefault("cpu", [])
    h.append((now, cpu))
    while len(h) > 2 and h[1][0] <= now - CPU_WIN:
        h.pop(0)
    return h[0][0] <= now - CPU_WIN + POLL and cpu - h[0][1] < CPU_IDLE


def sweep(verbose=False, status=False):
    now = time.time()
    susp = marks(DIR)
    js = marks(JDIR)
    procs = darling_pids()
    for _, exe, start, _ in procs.values():   # relaunched soon after its kill: something needs it, back off
        k = _killed_at.get(exe)
        if k is not None and k < start < k + RELAUNCH:
            _kills[exe] = _kills.get(exe, 0) + 1
            _killed_at[exe] = None
            print("%s jetsam: %s came back %.0f s after its kill; grace now x%d" % (
                time.strftime("%H:%M:%S"), name_of(exe), start - k, 2 ** _kills[exe]), flush=True)
        elif k is not None and start >= k + RELAUNCH:
            _killed_at[exe] = None
    live = {(d, p[2]) for d, p in procs.items()}
    for key in list(_seen):
        if key not in live:
            del _seen[key]
    rows = []
    for dpid, (lpid, exe, start, cpu) in procs.items():
        st = _seen.setdefault((dpid, start), {})
        sm = susp.get(dpid)
        sm = sm if sm is not None and sm[0] + 1 >= start else None   # (a mark older than the process: a reused pid's)
        jm = js.get(dpid)
        jm = jm if jm is not None and jm[0] + 1 >= start else None
        rows.append((dpid, lpid, exe, st, sm, jm, cpu_idle(st, now, cpu)))
    apps, killed = [], []
    fg_app = False
    for dpid, lpid, exe, st, sm, jm, idle in rows:
        if ".app/" not in exe or ".appex/" in exe or any(k in exe for k in NEVER):
            continue
        bg = jm[1][3] if jm and len(jm[1]) > 3 else -1
        role = jm[1][4] if jm and len(jm[1]) > 4 else -1
        if sm or bg == 1 or role in (3, 6):
            apps.append((min(sm[0] if sm else now, st.setdefault("bg", now) if (bg == 1 or role in (3, 6)) else now), lpid, dpid, exe))
            continue
        # not backgrounded by RunningBoard: in the foreground, unless SpringBoard says another app (or home) is
        if now - st.setdefault("fg", now) > APP:
            front = frontmost(now)
            if front is not None and front != bundle_id(exe):
                apps.append((st.setdefault("off", now), lpid, dpid, exe))
                continue
        st.pop("off", None)
        st.pop("bg", None)
        fg_app = True
    apps.sort(reverse=True)   # most recently backgrounded first
    for i, (since, lpid, dpid, exe) in enumerate(apps):
        g = grace(exe, LAST if i < KEEP else APP)
        if now - since > g:
            killed.append((lpid, dpid, exe, "app in the background %.0f s" % (now - since)))
        elif status:
            print("  app    %-40s %5.0f s of %.0f" % (name_of(exe), now - since, g))
    for dpid, lpid, exe, st, sm, jm, idle in rows:
        if ".appex/" in exe:
            if sm and now - sm[0] > grace(exe, EXT):
                killed.append((lpid, dpid, exe, "extension suspended %.0f s" % (now - sm[0])))
            continue
        if ".app/" in exe or not DAEMONS or not jm or len(jm[1]) < 2:
            continue
        dirty, name = jm[1][1], name_of(exe)
        if not (dirty & 1) or not ((dirty & 2) or name in DEMAND):
            continue
        if (dirty & 4) or not idle:
            st.pop("clean", None)
            continue
        since = max(jm[0], st.setdefault("clean", now))   # clean since its last dirty flip and since we saw it idle
        g = grace(exe, IDLE_HOT if (name in HOT or fg_app) else IDLE)
        if now - since > g:
            killed.append((lpid, dpid, exe, "daemon clean and idle %.0f s" % (now - since)))
        elif status:
            print("  daemon %-40s %5.0f s of %.0f" % (name, now - since, g))
    if status:
        print("  foreground app: %s" % fg_app)
        return []
    for lpid, dpid, exe, why in killed:
        try:
            os.kill(lpid, signal.SIGKILL)
            _killed_at[exe] = time.time()
            print("%s jetsam: killed %s (pid %d, %s)" % (time.strftime("%H:%M:%S"), name_of(exe), dpid, why), flush=True)
        except OSError:
            pass
    if verbose:
        print("%d suspended, %d tracked, %d killed" % (len(susp), len(js), len(killed)), flush=True)
    return killed


def main(a):
    if os.environ.get("X86SIM_JETSAM", "1") == "0":
        return
    if "--status" in a:   # what would be reclaimed and when (samples CPU over CPU_WIN first)
        t0 = time.time()
        while time.time() < t0 + CPU_WIN + POLL:
            for dpid, (lpid, exe, start, cpu) in darling_pids().items():
                cpu_idle(_seen.setdefault((dpid, start), {}), time.time(), cpu)
            time.sleep(POLL)
        sweep(status=True)
        return
    if "--once" in a:
        sweep("-v" in a)
        return
    while True:
        sweep("-v" in a)
        time.sleep(POLL)


if __name__ == "__main__":
    main(sys.argv[1:])
