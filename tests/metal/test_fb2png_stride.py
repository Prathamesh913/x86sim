#!/usr/bin/env python3
"""x86Sim Speed 1c: fb2png / ring decode must use IOSurface stride, not width*4.

A 1206-wide BGRA frame has tight rows of 4824 bytes and a 64-byte-aligned stride of 4864.
Decoding with Pillow frombuffer/frombytes without the real stride (or with w*4) shifts every
row by 40 bytes and produces thin diagonal lines — the artifact in docs/screens/arm64/31-speed1b-gpu-home.png.
"""
import os, struct, subprocess, sys, tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../cli/libexec"))
from PIL import Image
import numpy as np

W, H = 1206, 80
TIGHT = W * 4
STRIDE = (TIGHT + 63) & ~63  # 4864
assert STRIDE - TIGHT == 40


def make_padded_frame():
    """Vertical red bar at x=100, one pixel wide, on black; pad bytes = 0xCD."""
    buf = bytearray(STRIDE * H)
    for y in range(H):
        row = y * STRIDE
        for x in range(W):
            o = row + x * 4
            if x == 100:
                buf[o : o + 4] = b"\x00\x00\xff\xff"  # BGRA red
            else:
                buf[o : o + 4] = b"\x00\x00\x00\xff"
        for p in range(STRIDE - TIGHT):
            buf[row + TIGHT + p] = 0xCD
    return bytes(buf)


def decode(px, stride):
    return np.array(Image.frombuffer("RGBA", (W, H), px, "raw", "BGRA", stride, 1).convert("RGB"))


def main():
    px = make_padded_frame()
    ok = decode(px, STRIDE)
    # Correct: red bar stays at x=100 on every row
    for y in range(H):
        assert ok[y, 100, 0] == 255 and ok[y, 100, 1] == 0, (y, ok[y, 100])
        assert ok[y, 99].sum() == 0 and ok[y, 101].sum() == 0, y

    # Wrong decode (the Speed 1b screenshot bug): slope = pad/4 = 10
    wrong = np.empty((H, W, 3), dtype=np.uint8)
    for y in range(H):
        row = np.frombuffer(px[y * TIGHT : y * TIGHT + TIGHT], dtype=np.uint8).reshape(W, 4)
        wrong[y] = row[:, [2, 1, 0]]
    ys, xs = np.where(wrong[:, :, 0] > 200)
    assert len(ys) > 10
    slope = (xs[-1] - xs[0]) / (ys[-1] - ys[0])
    assert abs(slope - 10.0) < 0.05, slope

    # fb2png refuses a short buffer (tight-only bytes with a padded stride header)
    scratch = os.environ.get("X86SIM_SCRATCH", os.path.expanduser("~/scratch/x86sim"))
    root = os.path.join(os.path.dirname(__file__), "../..")
    fb2png = os.path.join(root, "cli/libexec/fb2png.py")
    with tempfile.TemporaryDirectory(dir=scratch) as td:
        dump = os.path.join(td, "frame.bgra")
        out = os.path.join(td, "out.png")
        # header claims stride 4864 but only tight*h pixels follow
        open(dump, "wb").write(struct.pack("<4I", W, H, STRIDE, 1) + px[: TIGHT * H])
        r = subprocess.run([sys.executable, fb2png, dump, out], capture_output=True, text=True)
        assert r.returncode == 2, (r.returncode, r.stderr)
        assert "refusing wrong-stride" in r.stderr
        # full padded buffer saves a straight bar
        open(dump, "wb").write(struct.pack("<4I", W, H, STRIDE, 1) + px)
        r = subprocess.run([sys.executable, fb2png, dump, out], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        got = np.array(Image.open(out))
        for y in range(H):
            assert got[y, 100, 0] == 255, y
    print("PASS test_fb2png_stride (1206 tight=%d stride=%d pad=%d)" % (TIGHT, STRIDE, STRIDE - TIGHT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
