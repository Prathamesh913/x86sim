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


def main():
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
