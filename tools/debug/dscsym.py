#!/usr/bin/env python3
# Symbolize / disassemble addresses in the arm64 iOS-simulator dyld shared cache (arm64 plan, stage 1).
# Usage: dscsym.py <cache file (main)> <addr> [ninsns]   (python3 -I; needs llvm-mc for disassembly)
# Prints the image and the nearest exported symbol at or below each address; with ninsns, disassembles from addr.
import mmap, struct, subprocess, sys

def uleb(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1; r |= (c & 0x7f) << s; s += 7
        if c < 0x80: return r, i

class Cache:
    def __init__(self, base):
        self.files = []
        for suf in [''] + ['.%02d' % i for i in range(1, 20)]:
            try: f = open(base + suf, 'rb')
            except OSError: break
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            mo, mc = struct.unpack_from('<II', m, 0x10)
            self.files.append((m, [struct.unpack_from('<QQQ', m, mo + 32 * i) for i in range(mc)]))
        m0 = self.files[0][0]
        io, ic = struct.unpack_from('<II', m0, 0x1C0)
        self.images = []
        for i in range(ic):
            addr, _, _, po, _ = struct.unpack_from('<QQQII', m0, io + 32 * i)
            self.images.append((addr, m0[po:m0.find(b'\0', po)].decode()))
        self.images.sort()
    def rd(self, addr, n):
        for m, maps in self.files:
            for a, sz, fo in maps:
                if a <= addr < a + sz: return m[fo + addr - a: fo + addr - a + n]
        raise KeyError(hex(addr))
    def image_of(self, addr):
        best = None
        for a, p in self.images:
            if a <= addr: best = (a, p)
            else: break
        return best
    def exports(self, mh):
        ncmds, sz = struct.unpack_from('<II', self.rd(mh, 32), 16)
        cmds = self.rd(mh + 32, sz); off = 0; le = text = None; tr = None
        for _ in range(ncmds):
            c, cs = struct.unpack_from('<II', cmds, off)
            if c == 0x19:
                name = cmds[off + 8:off + 24].split(b'\0')[0]
                vm, vs, fo = struct.unpack_from('<QQQ', cmds, off + 24)
                if name == b'__LINKEDIT': le = vm - fo
                if name == b'__TEXT': text = (vm, vs)
            if c == 0x80000033: tr = struct.unpack_from('<II', cmds, off + 8)
            off += cs
        out = []
        if tr:
            b = self.rd(le + tr[0], tr[1])
            def walk(i, pre):
                ts, i2 = uleb(b, i)
                if ts:
                    fl, j = uleb(b, i2)
                    if fl & 3 == 0 and not fl & 8:
                        v, _ = uleb(b, j); out.append((mh + v, pre))
                k = i2 + ts; n = b[k]; k += 1
                for _ in range(n):
                    e = b.index(b'\0', k); s = b[k:e].decode(); k = e + 1
                    o, k = uleb(b, k); walk(o, pre + s)
            walk(0, '')
        return sorted(out), text

def main():
    c = Cache(sys.argv[1]); addr = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    img = c.image_of(addr)
    exps, text = c.exports(img[0])
    sym = None
    for a, s in exps:
        if a <= addr: sym = (a, s)
    in_text = text and text[0] <= addr < text[0] + text[1]
    print('%#x: %s %s' % (addr, img[1], ('%s+%#x' % (sym[1], addr - sym[0])) if sym and in_text else '(no export below / not in __TEXT)'))
    if n:
        code = c.rd(addr, 4 * n)
        txt = ' '.join('0x%02x' % x for x in code)
        r = subprocess.run(['llvm-mc', '--disassemble', '-triple=arm64-apple-ios'], input=txt, capture_output=True, text=True)
        for i, l in enumerate([l for l in r.stdout.splitlines() if l.strip() and not l.strip().startswith('.')]):
            print('  %#x: %s' % (addr + 4 * i, l.strip()))

if __name__ == "__main__": main()
