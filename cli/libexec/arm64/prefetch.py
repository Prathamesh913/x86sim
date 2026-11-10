#!/usr/bin/env python3
# x86Sim (Speed 11): warm the host page cache for an arm64 boot. The host runs under memory pressure, and the boot's
# hot file pages (the AOT image .map/.idx and the dyld cache, ~0.9 GB) are often evicted between boots. They then come
# back as thousands of small random page faults from the encrypted disk while dyld_sim and the first processes wait
# (SpringBoard's dyld prepare took 3.5-12 s depending on it).
#
#   prefetch.py record HOTFILE FILE...   save the ranges of each FILE that running processes have mapped in (pagemap)
#   prefetch.py apply HOTFILE...          posix_fadvise(WILLNEED) on those ranges: large sequential reads, in the order
#                                         saved, started before launchd_sim (inside the boot's IO-capped scope)
#   prefetch.py record-seq HOTFILE SECS STEP FILE...
#                                         (Speed 14) snapshot every STEP s for SECS s and save the pages by the time
#                                         they first showed up (then by offset): the boot's first-touch order
#   prefetch.py read HOTFILE...           read those ranges in the saved order (real reads, so the order holds)
# Files that changed size since the record are skipped. No host setting is touched.
import json, os, sys

PAGE = os.sysconf("SC_PAGE_SIZE")
GAP = 64 << 10     # merge used runs closer than this (one read instead of many)


def used_pages(paths):
    """{path: set of file page indexes} present in some process's mapping of that file (/proc/PID/pagemap bit 63)"""
    want = {os.path.realpath(p): p for p in paths if not p.endswith("/")}
    dirs = tuple(os.path.realpath(p) + "/" for p in paths if p.endswith("/"))  # every mapped file under these
    used = {p: set() for p in want.values()}
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            maps = open(f"/proc/{pid}/maps").read().splitlines()
            pm = None
            for l in maps:
                f = l.split(None, 5)
                if len(f) < 6:
                    continue
                if f[5] not in want:
                    if not (dirs and f[5].startswith(dirs)):
                        continue
                    want[f[5]] = f[5]
                    used[f[5]] = set()
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


def existing(files):
    """The files that exist (a shell glob that matched nothing arrives literally, e.g. jc/*.map before the image's
    first load has made its map: that crashed the recording, so the next boot prefetched nothing)."""
    return [f for f in files if f.endswith("/") or os.path.exists(f)]


def record(hot, files):
    files = existing(files)
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


def record_seq(hot, secs, step, files):
    files = existing(files)
    import time
    first = {}   # path -> {page: seconds since start when first present}
    t0 = time.monotonic()
    while True:
        t = time.monotonic() - t0
        for p, pages in used_pages(files).items():
            seen = first.setdefault(p, {})
            for i in pages:
                seen.setdefault(i, t)
        if t >= secs:
            break
        time.sleep(max(0.0, step - (time.monotonic() - t0 - t)))
    out = []
    for p, seen in first.items():
        size = os.path.getsize(p)
        # ranges per step bucket, so each range carries the time its pages were first needed
        bucket = {}
        for i, t in seen.items():
            bucket.setdefault(int(t // step), set()).add(i)
        rs = [[a, b, k * step] for k, pages in bucket.items() for a, b in ranges_of(pages, size)]
        out.append({"path": p, "size": size, "ranges": sorted([a, b] for a, b, _ in rs), "timed": rs})
        if len(seen) * PAGE < (4 << 20):
            continue
        print(f"prefetch: {os.path.basename(p)}: {len(seen) * PAGE >> 20} MB used by {secs:.0f} s, "
              + " ".join(f"{k * step:.0f}s:{len(v) * PAGE >> 20}" for k, v in sorted(bucket.items())))
    print(f"prefetch: {len(out)} files, {sum(len(v) for v in first.values()) * PAGE >> 20} MB by {secs:.0f} s")
    tmp = hot + ".new"
    with open(tmp, "w") as f:
        json.dump(out, f)
    os.replace(tmp, hot)


def read_ordered(hot):
    try:
        entries = json.load(open(hot))
    except (OSError, ValueError):
        return
    work = []
    for e in entries:
        try:
            fd = os.open(e["path"], os.O_RDONLY)
        except OSError:
            continue
        if os.fstat(fd).st_size != e["size"]:
            os.close(fd)
            continue
        work += [(t, fd, a, b) for a, b, t in e.get("timed", [[a, b, 0] for a, b in e["ranges"]])]
    total = 0
    buf = bytearray(1 << 20)
    for t, fd, a, b in sorted(work, key=lambda w: (w[0], w[1], w[2])):
        while a < b:
            n = os.preadv(fd, [memoryview(buf)[:min(len(buf), b - a)]], a)
            if n <= 0:
                break
            a += n
            total += n
    for fd in {w[1] for w in work}:
        os.close(fd)
    print(f"prefetch: read {total >> 20} MB")


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
    elif len(sys.argv) >= 6 and sys.argv[1] == "record-seq":
        record_seq(sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), sys.argv[5:])
    elif len(sys.argv) >= 3 and sys.argv[1] == "read":
        for h in sys.argv[2:]:
            read_ordered(h)
    elif len(sys.argv) >= 3 and sys.argv[1] == "apply":
        for h in sys.argv[2:]:
            apply(h)
    else:
        sys.exit(__doc__ or "usage: prefetch.py record HOTFILE FILE... | apply HOTFILE")
