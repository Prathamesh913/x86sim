#!/usr/bin/env python3
# x86Code (Speed 14): build <jitcache dir>/placement.txt, the fixed addresses of non-cache Mach-Os (frameworks, bundles
# and app executables outside the dyld cache) in the free top of the cache window [0x2ac000000, 0x2c0000000).
# libarm64jit places each listed file there (dyld_sim's reservation after opening it, or the main executable), so its
# translations persist by address like the cache's; a file that changed (size/mtime) or isn't listed loads as before.
#
#   mkplacement.py OUT [LIST]   the files the running sim's processes have mapped, plus the paths in LIST (one per
#                               line; e.g. a previous table's keys), packed in path order
# Any change of the table changes the JIT cache key (rebuild the AOT image afterwards).
import os, struct, sys

LO, HI = 0x2AC000000, 0x2C0000000
GUARD = 0x10000


def arm64_vmsize(path):
    """the span of the arm64 slice's segments (as libarm64jit/dyld reserve it), or None"""
    try:
        with open(path, "rb") as f:
            b = f.read(1 << 16)
    except OSError:
        return None
    off = 0
    if b[:4] == b"\xca\xfe\xba\xbe":
        n = struct.unpack_from(">I", b, 4)[0]
        for i in range(n):
            ct, _, o = struct.unpack_from(">iII", b, 8 + 20 * i)
            if ct == 0x0100000C:
                off = o
                break
        else:
            return None
        with open(path, "rb") as f:
            f.seek(off)
            b = f.read(1 << 16)
    if b[:4] != b"\xcf\xfa\xed\xfe" or struct.unpack_from("<i", b, 4)[0] != 0x0100000C:
        return None
    ncmds, sz = struct.unpack_from("<II", b, 16)
    p, lo, hi = 32, None, 0
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from("<II", b, p)
        if cmd == 0x19:
            vm, vs, fo, fs, mp, ip = struct.unpack_from("<QQQQii", b, p + 24)
            if not (ip == 0 and fs == 0):
                lo = vm if lo is None else min(lo, vm)
                hi = max(hi, vm + vs)
        p += cs
    return None if lo is None else hi - lo


def key_of(path):
    i = path.find("/RuntimeRoot/")
    if i >= 0:
        return path[i + len("/RuntimeRoot/"):]
    i = path.find("/Containers/")
    return path[i + 1:] if i >= 0 else None


def live_files():
    out = set()
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            maps = open(f"/proc/{pid}/maps").read().splitlines()
        except OSError:
            continue
        for l in maps:
            f = l.split(None, 5)
            if len(f) == 6 and ("/RuntimeRoot/" in f[5] or "/Containers/Bundle/" in f[5]) \
                    and "dyld_sim_shared_cache" not in f[5]:
                out.add(f[5])
    return out


def main():
    out = sys.argv[1]
    files = live_files()
    root = None
    for p in files:
        if "/RuntimeRoot/" in p:
            root = p[:p.find("/RuntimeRoot/") + len("/RuntimeRoot/")]
    if len(sys.argv) > 2:
        for l in open(sys.argv[2]):
            k = l.split()[-1] if l.strip() else ""
            if k and root and not k.startswith("/"):
                files.add(root + k)
            elif k:
                files.add(k)
    rows = {}
    for p in files:
        k, vs = key_of(p), arm64_vmsize(p)
        if not k or not vs or not os.path.exists(p):
            continue
        st = os.stat(p)
        rows[k] = (vs, st.st_size, int(st.st_mtime))
    a, lines = LO, []
    for k in sorted(rows):
        vs, fs, mt = rows[k]
        slot = (vs + 0x3FFF) & ~0x3FFF
        if a + slot > HI:
            print(f"mkplacement: window full at {k}", file=sys.stderr)
            break
        lines.append(f"{a:x} {slot:x} {fs} {mt} {k}\n")
        a = (a + slot + GUARD + 0xFFFF) & ~0xFFFF
    tmp = out + ".new"
    with open(tmp, "w") as f:
        f.writelines(lines)
    os.replace(tmp, out)
    print(f"mkplacement: {len(lines)} files, {(a - LO) >> 20} MB of {(HI - LO) >> 20} MB")


if __name__ == "__main__":
    main()
