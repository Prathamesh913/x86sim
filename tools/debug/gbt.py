#!/usr/bin/env python3
# Input 3: guest backtraces of running arm64 processes (libarm64jit X86CODE_A64_GBT=1, e.g. through the a64-penv file:
# "SpringBoard X86CODE_A64_GBT=1"). Asks every such process for a dump (touches <scratch>/gbt/req; each process's host
# thread writes <scratch>/gbt/<exe>.<pid>.txt within a second) and prints the newest dump of each, cache addresses
# symbolized with dscsym.py (image + nearest export: an internal function shows as "<export below>+off").
#   python3 -I tools/debug/gbt.py [-n] [exe ...]     (-n: no new request, symbolize the last dumps)
import bisect, glob, os, re, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dscsym  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRATCH = os.environ.get('X86SIM_SCRATCH', os.path.expanduser('~/scratch/x86code'))
D = os.path.join(SCRATCH, 'gbt')
CACHE = os.environ.get('GBT_CACHE') or glob.glob(ROOT + '/runtimes/ios-27.2-24B8117-arm64/extracted/Library/Developer/CoreSimulator/Profiles/Runtimes/*/Contents/Resources/RuntimeRoot/System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_arm64')[0]

FILES = {}
def macho_syms(path):
    """(text vmaddr, sorted [(vmaddr, name)]) of a thin or fat arm64 Mach-O, from LC_SYMTAB."""
    if path in FILES: return FILES[path]
    r = (0, [])
    try:
        b = open(path, 'rb').read()
        base = 0
        if struct.unpack_from('>I', b, 0)[0] == 0xcafebabe:
            for i in range(struct.unpack_from('>I', b, 4)[0]):
                ct, _, off, _, _ = struct.unpack_from('>IIIII', b, 8 + 20 * i)
                if ct == 0x0100000c: base = off
        ncmds = struct.unpack_from('<I', b, base + 16)[0]; o = base + 32; text = 0; syms = []
        for _ in range(ncmds):
            c, cs = struct.unpack_from('<II', b, o)
            if c == 0x19 and b[o + 8:o + 24].split(b'\0')[0] == b'__TEXT': text = struct.unpack_from('<Q', b, o + 24)[0]
            if c == 0x2:
                so, ns, stro, _ = struct.unpack_from('<IIII', b, o + 8)
                for k in range(ns):
                    sx, ty, sect, _, v = struct.unpack_from('<IBBHQ', b, base + so + 16 * k)
                    if sect and not ty & 0xe0 and v:
                        e = b.index(b'\0', base + stro + sx); syms.append((v, b[base + stro + sx:e].decode(errors='replace')))
            o += cs
        r = (text, sorted(syms))
    except Exception: pass
    FILES[path] = r
    return r

def host_maps(hpid):
    out = []
    try:
        for l in open('/proc/%s/maps' % hpid):
            f = l.split(None, 5)
            lo, hi = (int(x, 16) for x in f[0].split('-'))
            out.append((lo, hi, int(f[2], 16), f[5].strip() if len(f) > 5 else ''))
    except OSError: pass
    return out

def main():
    args = sys.argv[1:]
    if args[:1] == ['-n']: args = args[1:]
    else:
        os.makedirs(D, exist_ok=True)
        open(os.path.join(D, 'req'), 'a').close(); os.utime(os.path.join(D, 'req'))
        time.sleep(2.5)
    c = dscsym.Cache(CACHE); ex = {}
    def sym(a):
        img = c.image_of(a)
        if not img: return ''
        if img[0] not in ex: ex[img[0]] = c.exports(img[0])
        exps, text = ex[img[0]]; s = None
        for e, n in exps:
            if e <= a: s = (e, n)
            else: break
        name = img[1].rsplit('/', 1)[-1]
        return '%s %s' % (name, '%s+%#x' % (s[1], a - s[0]) if s and text and text[0] <= a < text[0] + text[1] else '?')
    for f in sorted(glob.glob(os.path.join(D, '*.txt'))):
        exe = os.path.basename(f).rsplit('.', 2)[0]
        if args and exe not in args: continue
        txt = open(f).read(); last = txt[txt.rfind('=== gbt'):]
        mp = []
        for l in last.splitlines():
            h = re.match(r'--- tid \d+ \((\d+)/task', l)
            if h and not mp: mp = host_maps(h.group(1))
            m = re.match(r'(\s+#-?\d+\s+)0x([0-9a-f]+)\s*$', l)
            if m:
                a = int(m.group(2), 16); w = ''
                for lo, hi, off, path in mp:
                    if lo <= a < hi and path and 'dyld_sim_shared_cache' not in path:
                        text, syms = macho_syms(path); vm = text + off + a - lo
                        i = bisect.bisect_right(syms, (vm, '\xff')) - 1
                        w = '%s+%#x %s' % (path.rsplit('/', 1)[-1], off + a - lo, '%s+%#x' % (syms[i][1], vm - syms[i][0]) if i >= 0 else '')
                        break
                if not w and 0x1c0000000 <= a < 0x300000000: w = sym(a)
                l = '%s0x%s %s' % (m.group(1), m.group(2), w)
            print(l)

if __name__ == '__main__': main()
