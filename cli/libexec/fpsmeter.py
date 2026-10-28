#!/usr/bin/env python3
"""x86Code: measure how often simfb publishes frames (delivered fps), polling every 2 ms.
    cli/libexec/fpsmeter.py <seconds> [label]   prints per-0.5 s-window fps, the peak 1 s fps and frame-gap stats
Source: the shm ring's seq if there is one, else framebuffer.bgra's header seq (the pre-3.13 dump)."""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbring

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10
label = sys.argv[2] if len(sys.argv) > 2 else ""
ring = fbring.Ring()
use_ring = ring.seq() > 0


def cur():
    if use_ring:
        return ring.seq()
    try:
        with open(fbring.FILE, "rb") as f:
            return struct.unpack("<4I", f.read(16))[3]
    except (OSError, struct.error):
        return None


st0 = ring.stats() if use_ring else None
times = []
last = cur()
t_end = time.monotonic() + secs
while time.monotonic() < t_end:
    s = cur()
    if s is not None and s != last:
        times.append((time.monotonic(), s - last if last is not None and s > last else 1))
        last = s
    time.sleep(0.002)
if not times:
    print("%s: no frames" % label)
    sys.exit()
t0 = times[0][0]
frames = sum(n for _, n in times)
gaps = sorted(b[0] - a[0] for a, b in zip(times, times[1:]))
best = 0
for i, (t, _) in enumerate(times):
    n = sum(k for tt, k in times[i:] if tt < t + 1.0)
    best = max(best, n)
src = "ring" if use_ring else "file"
st1 = ring.stats() if use_ring else None
extra = ""
if st0 and st1 and st1["pid"] == st0["pid"]:
    d = {k: st1[k] - st0[k] for k in fbring.Ring.STATS}
    span = times[-1][0] - t0 or 1
    extra = "\n    simfb: %d submits (%.1f/s), %d superseded, %d watchdog, %d copied; GPU wait avg %.1f ms (max ever %.1f); copy avg %.1f ms" % (
        d["submits"], d["submits"] / secs, d["superseded"], d["watchdog"], d["copied"],
        d["wait_ns"] / max(d["copied"], 1) / 1e6, st1["wait_max_ns"] / 1e6, d["copy_ns"] / max(d["copied"], 1) / 1e6)
print("%s [%s]: %d frames seen (%d published) in %.1f s; peak 1 s window %d fps; gaps ms p50 %.1f p90 %.1f max %.1f" % (
    label, src, len(times), frames, times[-1][0] - t0, best,
    1000 * gaps[len(gaps) // 2] if gaps else 0, 1000 * gaps[int(len(gaps) * .9)] if gaps else 0,
    1000 * gaps[-1] if gaps else 0) + extra)
