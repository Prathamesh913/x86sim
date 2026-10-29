#!/usr/bin/env python3
"""x86Code: coarse checks of the simulator's newest frame (shm ring, cli/libexec/fbring.py), for boot waits and the
smoke test. Samples a 118x256 grid of the frame.
    framecheck.py stats                 print seq, dark/bright fractions (luma < 30 / > 200), mean saturation
    framecheck.py home                  exit 0 if the frame looks like the home screen: not the boot logo (~98% dark)
                                        and either colourful wallpaper (mean sat > 25) or enough bright pixels for the
                                        black-wallpaper software-render case (icons/dock, bright > 5%)
    framecheck.py app [ref.json]        exit 0 if the frame looks like a light-mode app page (> 60% bright) and,
                                        with ref.json (from `stats`), differs from that frame (mean abs diff > 20)
    framecheck.py wait home|app <secs> [ref.json]
                                        poll every 2 s until the check passes (exit 1 on timeout)"""
import json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbring

GW, GH = 118, 256
# Optional ring path: argv "... framecheck.py [/path/to.ring] stats|home|..." or SIMFB_RING_LINUX.
# Arm64 boots use /dev/shm/x86code-a64-simfb.ring; the fbring default is the x86 ring.
_RING = None


def _frame():
    if _RING:
        return fbring.Ring(_RING).latest()
    return fbring.latest_frame()


def sample():
    f = _frame()
    if not f:
        return None
    w, h, stride, seq, t, px = f if len(f) == 6 else (*f[:4], None, f[4])
    pts = []
    for j in range(GH):
        y = (2 * j + 1) * h // (2 * GH)
        row = y * stride
        for i in range(GW):
            o = row + ((2 * i + 1) * w // (2 * GW)) * 4
            b, g, r = px[o], px[o + 1], px[o + 2]
            pts.append((r, g, b))
    return seq, pts


def stats():
    s = sample()
    if not s:
        return None
    seq, pts = s
    n = len(pts)
    luma = [(r + g + b) / 3 for r, g, b in pts]
    return {"seq": seq, "dark": sum(l < 30 for l in luma) / n, "bright": sum(l > 200 for l in luma) / n,
            "sat": sum(max(p) - min(p) for p in pts) / n, "grid": [round(l) for l in luma[::7]]}


def check(kind, ref=None):
    st = stats()
    if not st:
        return False, "no frame"
    if kind == "home":
        # Colourful wallpaper, or black wallpaper with icons/dock (stage 2/3 arm64 software render: ~21% bright, ~74% dark).
        ok = st["dark"] < 0.95 and (st["sat"] > 25 or st["bright"] > 0.05)
    else:
        ok = st["bright"] > 0.6
        if ok and ref:
            g1, g2 = st["grid"], ref["grid"]
            diff = sum(abs(a - b) for a, b in zip(g1, g2)) / max(1, len(g1))
            st["diff"] = diff
            ok = diff > 20
    st.pop("grid")
    return ok, json.dumps({k: round(v, 3) if isinstance(v, float) else v for k, v in st.items()})


def main():
    global _RING
    a = sys.argv[1:]
    # Leading path to a .ring file (arm64: /dev/shm/x86code-a64-simfb.ring) selects the ring.
    if a and a[0].endswith(".ring"):
        _RING = a[0]
        a = a[1:]
    if not a or a[0] == "stats":
        st = stats()
        print(json.dumps(st) if st else "no frame")  # (grid: sampled luma, for `app ref.json`)
        return 0 if st else 1
    if a[0] == "wait":
        kind, secs = a[1], float(a[2])
        ref = json.load(open(a[3])) if len(a) > 3 else None
        end = time.time() + secs
        while True:
            ok, info = check(kind, ref)
            if ok or time.time() > end:
                print(info)
                return 0 if ok else 1
            time.sleep(2)
    ref = json.load(open(a[1])) if len(a) > 1 else None
    ok, info = check(a[0], ref)
    print(info)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
