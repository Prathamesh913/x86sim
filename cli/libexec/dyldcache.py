#!/usr/bin/env python3
"""The simulator's x86_64 dyld shared cache (docs/notebook/FINDINGS.md (dyld shared cache)).

The cache is built once on a Mac from the 23F73 runtime (build/mac-build-dyldcache.sh) and copied to
runtimes/dyld-cache-23F73-x86_64/. dyld_sim finds it through DYLD_SHARED_CACHE_DIR (sim.env) and maps it privately at
its fixed addresses (0x7FF800000000, no slide); Darling's mldr keeps that range free.

The cache records each dylib's inode and mtime; dyld_sim treats a dylib whose file on disk has a different FileID as a
root that overrides the cache and loads the file instead. The inodes are the Mac's, so `localize` writes this host's
inodes (and mtimes) of the extracted runtime into the cache's image table, as CoreSimulator does by building one cache
per host. Run it again after re-extracting the runtime.

  dyldcache.py check    [cache dir] [runtime root]   how many images match the files on disk
  dyldcache.py localize [cache dir] [runtime root]   rewrite the image table to match (in place, idempotent)
  dyldcache.py compare  <cache dir> <reference dir>  layout, mappings, dylib list/addresses and bytes of each file
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
import x86config  # noqa: E402

CACHE = "dyld_sim_shared_cache_x86_64"
IMAGE_INFO = struct.Struct("<QQQII")  # dyld_cache_image_info: address, modTime, inode, pathFileOffset, pad


def default_dirs():
    root = x86config.ROOT
    return (os.path.join(root, "runtimes", "dyld-cache-23F73-x86_64"),
            x86config.get("X86SIM_RUNTIME"))


def images(f):
    f.seek(0)
    h = f.read(0x200)
    if not h.startswith(b"dyld_v1  x86_64"):
        sys.exit("not an x86_64 dyld cache")
    off, count = struct.unpack_from("<II", h, 0x1C0)  # imagesOffset, imagesCount
    f.seek(off)
    table = f.read(IMAGE_INFO.size * count)
    for i in range(count):
        addr, mtime, inode, path_off, _ = IMAGE_INFO.unpack_from(table, i * IMAGE_INFO.size)
        f.seek(path_off)
        path = f.read(1024).split(b"\0", 1)[0].decode()
        yield off + i * IMAGE_INFO.size, mtime, inode, path


def layout(path):
    with open(path, "rb") as f:
        h = f.read(0x400)
        mo, mc = struct.unpack_from("<II", h, 0x138)  # mappingWithSlideOffset, count
        maps = [struct.unpack_from("<QQQQQQII", h, mo + i * 56) for i in range(mc)]
        return {"magic": h[:16], "uuid": h[0x58:0x68].hex(), "platform": struct.unpack_from("<I", h, 0xD8)[0],
                "region": struct.unpack_from("<QQ", h, 0xE0), "mappings": maps}


def compare(a, b):
    """Exit 0 if the two caches have the same layout and dylibs (addresses included); report byte differences."""
    names = sorted(n for n in os.listdir(b) if n.startswith(CACHE) and not n.endswith((".map", ".atlas")))
    bad = 0
    for n in names:
        pa, pb = os.path.join(a, n), os.path.join(b, n)
        if not os.path.exists(pa):
            print(f"{n}: missing"); bad += 1; continue
        la, lb = layout(pa), layout(pb)
        for k in ("magic", "platform", "region", "mappings"):
            if la[k] != lb[k]:
                print(f"{n}: {k} differs: {la[k]} != {lb[k]}"); bad += 1
        same = os.path.getsize(pa) == os.path.getsize(pb)
        diff = 0
        if same:
            with open(pa, "rb") as fa, open(pb, "rb") as fb:
                while True:
                    x, y = fa.read(1 << 22), fb.read(1 << 22)
                    if not x:
                        break
                    if x != y:
                        diff += sum(1 for i in range(0, len(x), 4096) if x[i:i + 4096] != y[i:i + 4096])
        print(f"{n}: size {'same' if same else 'differs'}, {diff} differing 4K pages, uuid {la['uuid'] == lb['uuid'] and 'same' or 'differs'}")
    with open(os.path.join(a, CACHE), "rb") as fa, open(os.path.join(b, CACHE), "rb") as fb:
        ia = [(p, addr) for addr, p in ((e[0], e[3]) for e in images_addr(fa))]
        ib = [(p, addr) for addr, p in ((e[0], e[3]) for e in images_addr(fb))]
    if ia != ib:
        sa, sb = set(ia), set(ib)
        print(f"dylibs differ: {len(sa - sb)} only here, {len(sb - sa)} only in the reference"); bad += 1
    else:
        print(f"{len(ia)} dylibs, same paths and addresses")
    sys.exit(1 if bad else 0)


def images_addr(f):
    f.seek(0)
    off, count = struct.unpack_from("<II", f.read(0x200), 0x1C0)
    f.seek(off)
    table = f.read(IMAGE_INFO.size * count)
    for i in range(count):
        addr, mtime, inode, path_off, _ = IMAGE_INFO.unpack_from(table, i * IMAGE_INFO.size)
        f.seek(path_off)
        yield addr, mtime, inode, f.read(1024).split(b"\0", 1)[0].decode()


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "compare":
        compare(sys.argv[2], sys.argv[3])
    if len(sys.argv) < 2 or sys.argv[1] not in ("check", "localize"):
        sys.exit(__doc__)
    cache_dir, rt = default_dirs()
    if len(sys.argv) > 2:
        cache_dir = sys.argv[2]
    if len(sys.argv) > 3:
        rt = sys.argv[3]
    write = sys.argv[1] == "localize"
    total = match = changed = missing = 0
    with open(os.path.join(cache_dir, CACHE), "r+b" if write else "rb") as f:
        for entry, mtime, inode, path in list(images(f)):
            total += 1
            try:
                st = os.stat(rt + path)
            except FileNotFoundError:
                missing += 1
                continue
            want = (int(st.st_mtime), st.st_ino)
            if want == (mtime, inode):
                match += 1
            elif write:
                f.seek(entry + 8)
                f.write(struct.pack("<QQ", *want))
                changed += 1
    print(f"{total} images: {match} match, {changed} rewritten, {missing} not on disk")


if __name__ == "__main__":
    main()
