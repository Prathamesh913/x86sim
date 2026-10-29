#!/usr/bin/env python3
"""Convert the simulator's newest frame to PNG: from host/simfb's shared-memory ring (cli/libexec/fbring.py) when the
source is the default framebuffer.bgra (or "ring"), else from the given dump file ({w, h, stride, seq} + BGRA rows).
    cli/libexec/fb2png.py [framebuffer.bgra|ring|file] [out.png] [scale-divisor]"""
import os, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "lib"))
import x86config  # noqa: E402
import fbring

src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(x86config.data(), "framebuffer.bgra")
dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.environ.get("X86SIM_SCRATCH", "/home/bobby/scratch/x86code"), "frame.png")
div = int(sys.argv[3]) if len(sys.argv) > 3 else 1
# x86Code: framebuffer.bgra is only rewritten every 2 s now; the ring has every frame.
# Arm64 boots: pass /dev/shm/x86code-a64-simfb.ring (or set SIMFB_RING_LINUX and use "ring").
if src.endswith(".ring"):
    f = fbring.Ring(src).latest()
elif src == "ring" or os.path.basename(src) == "framebuffer.bgra":
    f = fbring.latest_frame()
else:
    f = None
if f:
    # Ring.latest → (w,h,stride,seq,t,px); latest_frame → (w,h,stride,seq,px)
    w, h, stride, seq, px = (f[0], f[1], f[2], f[3], f[5]) if len(f) == 6 else (*f[:4], f[4])
else:
    data = open(src, "rb").read()
    w, h, stride, seq = struct.unpack("<4I", data[:16])
    px = data[16:]
try:   # PIL when there is one (fast), else the pure-Python encoder below
    from PIL import Image
    img = Image.frombuffer("RGBA", (w, h), px, "raw", "BGRA", stride, 1).convert("RGB")
    if div > 1:
        img = img.reduce(div)
    img.save(dst)
    print("frame %d: %dx%d stride %d -> %s (%dx%d)" % (seq, w, h, stride, dst, img.width, img.height))
    sys.exit()
except ImportError:
    pass
ow, oh = w // div, h // div
rows = bytearray()
for y in range(oh):
    rows.append(0)
    row = px[y * div * stride:]
    for x in range(ow):
        b, g, r, a = row[x * div * 4:x * div * 4 + 4]
        rows += bytes((r, g, b))
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", ow, oh, 8, 2, 0, 0, 0)) \
    + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b"")
open(dst, "wb").write(png)
print("frame %d: %dx%d stride %d -> %s (%dx%d)" % (seq, w, h, stride, dst, ow, oh))
