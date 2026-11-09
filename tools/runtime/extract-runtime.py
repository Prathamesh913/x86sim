#!/usr/bin/env python3
"""Extract Apple's iOS simulator runtime (the exportedBundle's iOSSimulatorRuntime_Cryptex.dmg) on Linux.

    tools/runtime/extract-runtime.py <bundle dir or .dmg> <out dir>        extract, then fix symlinks
    tools/runtime/extract-runtime.py --check <bundle dir or .dmg> <out dir> only compare symlinks, change nothing
    tools/runtime/extract-runtime.py --links <bundle dir or .dmg> <out dir> only fix symlinks (after an extraction)

7-Zip reads the APFS image directly (`7z x -snld`). Two things need fixing afterwards (the 80 links of runtimes/<rt>/restored-symlinks.tsv were restored from a Mac by hand before this script):
- 7-Zip skips links whose target is another link ("Dangerous link via another link was ignored", exit code 2;
  e.g. usr/lib/libinfo.dylib -> libSystem.dylib -> libSystem.B.dylib) or writes them as empty files (older
  versions); each link of the image's listing (`7z l -slt`) is made again;
- absolute links (/System/Library/...) point into the runtime and are made relative to the link inside RuntimeRoot.
The runtime is Apple's and stays out of the repository (runtimes/ is gitignored)."""
import os, subprocess, sys

def dmg_of(p):
    if os.path.isdir(p):
        p = os.path.join(p, "Restore", "iOSSimulatorRuntime_Cryptex.dmg")
    if not os.path.isfile(p):
        sys.exit("no iOSSimulatorRuntime_Cryptex.dmg at " + p)
    return p

def links(dmg):
    out = subprocess.run(["7z", "l", "-slt", dmg], capture_output=True, text=True, errors="surrogateescape").stdout
    path = None
    for line in out.splitlines():
        if line.startswith("Path = "):
            path = line[7:]
        elif line.startswith("Symbolic Link = ") and path and line[16:]:
            yield path, line[16:]

def main():
    args = sys.argv[1:]
    check = bool(args) and args[0] == "--check"
    links_only = bool(args) and args[0] == "--links"
    if check or links_only:
        args = args[1:]
    if len(args) != 2:
        sys.exit(__doc__)
    dmg, out = dmg_of(args[0]), args[1]
    if not check and not links_only:
        os.makedirs(out, exist_ok=True)
        r = subprocess.run(["7z", "x", "-y", "-snld", "-o" + out, dmg])
        # 1 = warnings; 2 also when the only errors are "Dangerous link via another link was ignored" (7-Zip 26:
        # 6 links; earlier versions wrote 80 as empty files). Those links are made below either way.
        if r.returncode not in (0, 1, 2):
            sys.exit("7z failed: %d" % r.returncode)
    fixed = ok = 0
    for path, target in links(dmg):
        full = os.path.join(out, path)
        want = target
        rr = path.find("/RuntimeRoot/")
        if target.startswith("/") and rr >= 0:
            root = os.path.join(out, path[:rr + len("/RuntimeRoot")])
            want = os.path.relpath(root + target, os.path.dirname(full))
        if os.path.islink(full) and os.readlink(full) == want:
            ok += 1
            continue
        if check:
            kind = "link -> " + os.readlink(full) if os.path.islink(full) else (
                "empty file" if os.path.isfile(full) and os.path.getsize(full) == 0 else "missing" if not os.path.lexists(full) else "other")
            print("%s: %s, want -> %s" % (path, kind, want))
            fixed += 1
            continue
        if os.path.lexists(full) and not os.path.isdir(full) or os.path.islink(full):
            os.unlink(full)
        elif os.path.isdir(full):
            print("skipped (a directory is there): " + path)
            continue
        os.makedirs(os.path.dirname(full), exist_ok=True)
        os.symlink(want, full)
        fixed += 1
    print("symlinks: %d right, %d %s" % (ok, fixed, "wrong" if check else "made"))

main()
