#!/usr/bin/env python3
"""tests/arm64/gate/kblat.py (Input: host keyboard latency): typing latency key -> screen.
Types TEXT at RATE keys/s into the focused field (via simhid's socket, or with --wtype through the launcher window,
which must have keyboard focus), records the frame ring meanwhile and OCRs the field crop of each changed frame.
Per key: send time -> first frame whose field text is at least that long. Prints p50/p90/max and the final text.
    kblat.py RATE TEXT [--crop x0,y0,x1,y1] [--wtype] [--clear N]
Env: SIMHID_SOCKET, SIMFB_RING_LINUX."""
import hashlib, os, socket, subprocess, sys, threading, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../cli/libexec"))
import fbring  # noqa: E402

KEYS = {}
for i, c in enumerate("abcdefghijklmnopqrstuvwxyz"):
    KEYS[c] = (4 + i, False); KEYS[c.upper()] = (4 + i, True)
for i, c in enumerate("1234567890"):
    KEYS[c] = (0x1e + i, False)
for c, u, s in ((" ", 0x2c, 0), (".", 0x37, 0), (",", 0x36, 0), ("!", 0x1e, 1), ("?", 0x38, 1), ("'", 0x34, 0), ("-", 0x2d, 0)):
    KEYS[c] = (u, bool(s))


def main(a):
    rate, text = float(a[0]), a[1]
    crop = (230, 2390, 1000, 2490)
    wt = "--wtype" in a
    clear = 0
    for i, x in enumerate(a):
        if x == "--crop": crop = tuple(map(int, a[i + 1].split(",")))
        if x == "--clear": clear = int(a[i + 1])
    ring = fbring.Ring(os.environ.get("SIMFB_RING_LINUX", "/dev/shm/x86sim-a64-simfb.ring"))
    frames, stop = [], [False]
    x0, y0, x1, y1 = crop

    def grab():
        f = ring.latest()
        if not f: return None
        w, h, stride, seq, t, px = f
        rows = b"".join(px[y * stride + x0 * 4:y * stride + x1 * 4] for y in range(y0, y1))
        return seq, t, rows

    def rec():
        last = None
        while not stop[0]:
            s = ring.wait(last or 0, 0.2)
            if s == last: continue
            last = s
            g = grab()
            if g: frames.append(g)
    th = threading.Thread(target=rec); th.start()
    sk = None
    if not wt:
        sk = socket.socket(socket.AF_UNIX); sk.connect(os.environ["SIMHID_SOCKET"]); sk.sendall(b"kbidle 300\n" + (b"kbsettle %d\n" % int(os.environ["KBSETTLE"]) if os.environ.get("KBSETTLE") else b""))
    def send_char(c):
        if wt:
            subprocess.run(["wtype", "-d", "5", c] if c != "\b" else ["wtype", "-k", "BackSpace"])
            return
        u, sh = (0x2a, False) if c == "\b" else KEYS[c]
        m = (b"key down 225\n" if sh else b"") + b"key down %d\nkey up %d\n" % (u, u) + (b"key up 225\n" if sh else b"")
        sk.sendall(m)
    for _ in range(clear):
        send_char("\b"); time.sleep(0.03)
    time.sleep(float(os.environ.get("KBLAT_PRE", "1.5")))
    sends = []
    t0 = time.monotonic()
    text = text.replace("\\b", "\b")
    for i, c in enumerate(text):
        tgt = t0 + i / rate
        while time.monotonic() < tgt: time.sleep(0.001)
        sends.append(time.time_ns()); send_char(c)
    time.sleep(4)
    stop[0] = True; th.join()
    # OCR distinct crops
    W, H = x1 - x0, y1 - y0
    cache, seen = {}, []
    for seq, t, rows in frames:
        h = hashlib.md5(rows).hexdigest()
        if h not in cache:
            from PIL import Image
            im = Image.frombytes("RGBA", (W, H), rows, "raw", "BGRA").convert("L")
            p = "/tmp/kblat-crop.png"; im.save(p)
            o = subprocess.run(["tesseract", p, "stdout", "--psm", "7"], capture_output=True, text=True).stdout.strip()
            o = o.replace("|", "").strip(); cache[h] = "" if o.lower() in ("search", "q search") else o
        seen.append((t, cache[h]))
    final = seen[-1][1] if seen else ""
    exp = ""
    for c in text: exp = exp[:-1] if c == "\b" else exp + c
    lat = []
    for i, ts in enumerate(sends):
        n = i + 1
        hit = next((t for t, o in seen if t >= ts and len(o.replace(" ", "")) >= len(text[:n].replace(" ", ""))), None)
        lat.append((hit - ts) / 1e6 if hit else None)
    ok = [x for x in lat if x is not None]
    ok.sort()
    q = lambda p: ok[min(len(ok) - 1, int(p * len(ok)))] if ok else -1
    print("rate %.0f/s keys %d frames %d seen %d  p50 %.0f p90 %.0f max %.0f ms  last-key %s ms" % (
        rate, len(text), len(frames), len(ok), q(.5), q(.9), ok[-1] if ok else -1,
        "%.0f" % lat[-1] if lat[-1] is not None else "-"))
    print("final: %r  expect %r  %s" % (final, exp, "MATCH" if final.strip() == exp.strip() else "DIFF"))


if __name__ == "__main__":
    main(sys.argv[1:])
