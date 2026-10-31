#!/usr/bin/env python3
# x86Code (Speed 11): warm the host page cache for an arm64 boot. The host runs under memory pressure, and the boot's
# hot file pages (the AOT image .map/.idx and the dyld cache, ~0.9 GB) are often evicted between boots. They then come
# back as thousands of small random page faults from the encrypted disk while dyld_sim and the first processes wait
# (SpringBoard's dyld prepare took 3.5-12 s depending on it).
#
#   prefetch.py record HOTFILE FILE...   save the ranges of each FILE that running processes have mapped in (pagemap)
#   prefetch.py apply HOTFILE...          posix_fadvise(WILLNEED) on those ranges: large sequential reads, in the order
#                                         saved, started before launchd_sim (inside the boot's IO-capped scope)
# Files that changed size since the record are skipped. No host setting is touched.
import json, os, sys

PAGE = os.sysconf("SC_PAGE_SIZE")
GAP = 64 << 10     # merge used runs closer than this (one read instead of many)


def used_pages(paths):
    """{path: set of file page indexes} present in some process's mapping of that file (/proc/PID/pagemap bit 63)"""
    want = {os.path.realpath(p): p for p in paths}
    used = {p: set() for p in paths}
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            maps = open(f"/proc/{pid}/maps").read().splitlines()
            pm = None
            for l in maps:
                f = l.split(None, 5)
                if len(f) < 6 or f[5] not in want:
                    continue
                lo, hi = (int(x, 16) for x in f[0].split("-"))
                off = int(f[2], 16)
                if pm is None:
                    pm = open(f"/proc/{pid}/pagemap", "rb")
                pm.seek(lo // PAGE * 8)
                ents = pm.read((hi - lo) // PAGE * 8)
                pages = used[want[f[5]]]
                for i in range(len(ents) // 8):
                    if ents[i * 8 + 7] & 0x80:  # present
                        pages.add(off // PAGE + i)
            if pm:
                pm.close()
        except OSError:
            continue
    return used


def ranges_of(pages, size):
    ranges = []
    for i in sorted(pages):
        a, b = i * PAGE, min((i + 1) * PAGE, size)
        if ranges and a - ranges[-1][1] <= GAP:
            ranges[-1][1] = b
        else:
            ranges.append([a, b])
    return ranges


def record(hot, files):
    out = []
    for p, pages in used_pages(files).items():
        size = os.path.getsize(p)
        rs = ranges_of(pages, size)
        out.append({"path": p, "size": size, "ranges": rs})
        print(f"prefetch: {os.path.basename(p)}: {len(pages) * PAGE >> 20} MB used, {sum(b - a for a, b in rs) >> 20} MB in {len(rs)} ranges")
    tmp = hot + ".new"
    with open(tmp, "w") as f:
        json.dump(out, f)
    os.replace(tmp, hot)


def apply(hot):
    try:
        entries = json.load(open(hot))
    except (OSError, ValueError):
        return
    total = 0
    for e in entries:
        try:
            fd = os.open(e["path"], os.O_RDONLY)
        except OSError:
            continue
        try:
            if os.fstat(fd).st_size != e["size"]:
                continue
            for a, b in e["ranges"]:
                os.posix_fadvise(fd, a, b - a, os.POSIX_FADV_WILLNEED)
                total += b - a
        finally:
            os.close(fd)
    print(f"prefetch: requested {total >> 20} MB")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "record":
        record(sys.argv[2], sys.argv[3:])
    elif len(sys.argv) >= 3 and sys.argv[1] == "apply":
        for h in sys.argv[2:]:
            apply(h)
    else:
        sys.exit(__doc__ or "usage: prefetch.py record HOTFILE FILE... | apply HOTFILE")
