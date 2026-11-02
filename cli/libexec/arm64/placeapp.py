#!/usr/bin/env python3
# x86Code (Speed 31): give an installed app's Mach-Os (executable, its dylibs, embedded frameworks, plug-ins) slots in
# <jitcache dir>/placement.txt without moving any existing slot (mkplacement.py packs in path order, so a Containers/
# key would shift every System/ slot and force their image chunks to rebuild). New keys go after the highest slot; a
# reinstalled file (other size/mtime) keeps its slot when it still fits. Afterwards `x86code-aot update` builds the
# app tier's chunks (one per file, keyed by the Mach-O UUID + a hash of the file + its slot).
#   placeapp.py JC_DIR APP_DIR...   (APP_DIR: <device>/data/Containers/Bundle/Application/<UUID>/<Name>.app)
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkplacement import arm64_vmsize, key_of, HI, GUARD

def main():
    jc, apps = sys.argv[1], sys.argv[2:]
    path = os.path.join(jc, "placement.txt")
    lines = open(path).read().splitlines() if os.path.exists(path) else []
    rows = []  # [addr, slot, fsize, mtime, key]
    for l in lines:
        f = l.split(None, 4)
        if len(f) == 5:
            rows.append([int(f[0], 16), int(f[1], 16), int(f[2]), int(f[3]), f[4]])
    by_key = {r[4]: r for r in rows}
    end = max((r[0] + r[1] for r in rows), default=0x2AC000000)
    changed = 0
    # Speed 34: a reinstall gets a new container UUID; the old container's rows stayed in placement.txt forever.
    # Drop rows of the same .app name in another container when that container exists on no phone (the jitcache
    # dir is shared by every phone of this runtime: var/device-*/data and the installing phone's data are checked).
    # Remaining slots never move.
    datas = set()
    root = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
    vd = os.path.join(root, "var")
    if os.path.isdir(vd):
        datas |= {os.path.join(vd, d, "data") for d in os.listdir(vd) if d.startswith("device")}
    names = set()
    for app in apps:
        rp = os.path.realpath(app)
        i = rp.find("/Containers/")
        if i >= 0:
            datas.add(rp[:i])
            names.add(os.path.basename(rp))
    for r in list(rows):
        f = r[4].split("/")
        if len(f) > 5 and f[0] == "Containers" and f[4] in names and \
                not any(os.path.isdir(os.path.join(d, *f[:4])) for d in datas):
            rows.remove(r)
            by_key.pop(r[4], None)
            changed += 1
            print(f"placeapp: pruned stale {r[4]}")
    for app in apps:
        for d, _, fs in os.walk(os.path.realpath(app)):
            for n in sorted(fs):
                p = os.path.join(d, n)
                if os.path.islink(p) or not os.path.isfile(p):
                    continue
                vs, k = arm64_vmsize(p), key_of(p)
                if not vs or not k:
                    continue
                st = os.stat(p)
                slot = (vs + 0x3FFF) & ~0x3FFF
                r = by_key.get(k)
                if r and (r[2], r[3]) == (st.st_size, int(st.st_mtime)):
                    continue
                if r and slot <= r[1]:
                    r[2], r[3] = st.st_size, int(st.st_mtime)
                else:
                    if r:
                        rows.remove(r)
                    a = (end + GUARD + 0xFFFF) & ~0xFFFF
                    if a + slot > HI:
                        print(f"placeapp: window full, {k} ({slot >> 10} KB) stays unplaced", file=sys.stderr)
                        continue
                    r = [a, slot, st.st_size, int(st.st_mtime), k]
                    rows.append(r)
                    by_key[k] = r
                    end = a + slot
                changed += 1
                print(f"placeapp: {r[0]:x} {r[1] >> 10} KB {k}")
    if not changed:
        print("placeapp: nothing to place")
        return
    tmp = path + ".new"
    with open(tmp, "w") as f:
        f.writelines(f"{a:x} {s:x} {fs} {mt} {k}\n" for a, s, fs, mt, k in rows)
    os.replace(tmp, path)

if __name__ == "__main__":
    main()
