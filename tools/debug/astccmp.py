#!/usr/bin/env python3
"""x86Code: compare linux/mtlvk astctest's GPU decode (MTLSimDriver's astc kernel on metal2vulkan) with a
CPU ASTC decoder (texture2ddecoder, pip). Usage: astccmp.py <file.ktx> <bx0> <by0> <bw> <bh> <gpu.rgba> [out.png]"""
import struct, sys
import numpy as np, texture2ddecoder
from PIL import Image
f, bx0, by0, bw, bh, g = sys.argv[1], *map(int, sys.argv[2:6]), sys.argv[6]
d = open(f, 'rb').read()
w = struct.unpack('<I', d[36:40])[0]; kv = struct.unpack('<I', d[60:64])[0]
off = 64 + kv + 4; bpr = (w + 3) // 4 * 16
blk = b''.join(d[off + (by0 + y) * bpr + bx0 * 16: off + (by0 + y) * bpr + (bx0 + bw) * 16] for y in range(bh))
cpu = np.frombuffer(texture2ddecoder.decode_astc(blk, bw * 4, bh * 4, 4, 4), np.uint8).reshape(bh * 4, bw * 4, 4)[:, :, [2, 1, 0, 3]]
gpu = np.frombuffer(open(g, 'rb').read(), np.uint8).reshape(bh * 4, bw * 4, 4)
diff = np.abs(cpu.astype(int) - gpu.astype(int)).max(axis=2)
bad = diff > 2
bb = bad.reshape(bh, 4, bw, 4).any(axis=(1, 3))
print("texels off by >2: %d of %d; blocks bad: %d of %d; max diff %d" % (bad.sum(), bad.size, bb.sum(), bb.size, diff.max()))
for (y, x) in list(zip(*np.nonzero(bb)))[:8]:
    i = (y * bw + x) * 16
    print(" block (%d,%d) %s cpu %s gpu %s" % (x, y, blk[i:i + 16].hex(), cpu[y * 4, x * 4], gpu[y * 4, x * 4]))
if len(sys.argv) > 7:
    Image.fromarray(np.concatenate([cpu, gpu], axis=1), 'RGBA').save(sys.argv[7])
