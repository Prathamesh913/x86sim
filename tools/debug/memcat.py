#!/usr/bin/env python3
"""RAM 1: PSS of the running arm64 sim by mapping category and by process, from /proc/<pid>/smaps.

The sim = the darlingserver of A64_DPREFIX (default $X86SIM_SCRATCH/a64prefix) and all its descendants, plus mtlvk
(the same set as `x86code-aot sim status`). Categories:
  runtime-files   the runtime's files, mostly the dyld shared cache (clean, shared page cache)
  aot.map/.idx    the AOT image and its index (clean, shared; COW'd link patches show under pd)
  anon/<perm>     anonymous memory (guest heaps, Darling, libarm64jit/dynarmic); rwxp = JIT code
  heap            glibc's brk heap (libarm64jit/dynarmic/mldr; mtlvk)
  nvidia-*        the NVIDIA driver (mtlvk)
  memfd:*         Darling memory entries (IOSurfaces), frozen vm_remap copies, libsystem stubs
  a64:* / glibc:* named anonymous mappings when the sim runs with X86CODE_A64_VMANAME=1 and/or
                  A64_GLIBC_TUNABLES_EXTRA=glibc.mem.decorate_maps=1 (diagnostics)
Columns: PSS MB, SwapPss MB, pd = Private_Dirty MB (memory only this process has written).
Note (RAM 1): file-backed PSS is the page cache that processes have mapped; every read fault maps a 64 KB
fault-around window, so it is ~15x the pages actually touched (see docs/notebook/rounds/ram1.md).
  memcat.py [--json out.json]     TOP=N (processes listed, default 25), DETAIL=1 (top mappings by name)
"""
import collections
import json
import os
import re
import sys

SCR = os.environ.get("X86SIM_SCRATCH", os.path.expanduser("~/scratch/x86code"))
PREFIX = os.environ.get("A64_DPREFIX", os.path.join(SCR, "a64prefix"))


def rd(p):
    try:
        with open(p, "rb") as f:
            return f.read()
    except OSError:
        return b""


def sim_pids():
    kids = collections.defaultdict(list)
    comm = {}
    root = None
    extra = []
    for e in os.listdir("/proc"):
        if not e.isdigit():
            continue
        pid = int(e)
        c = rd(f"/proc/{pid}/comm").decode().strip()
        st = rd(f"/proc/{pid}/stat").decode()
        try:
            pp = int(st.rsplit(")", 1)[1].split()[1])
        except (IndexError, ValueError):
            continue
        kids[pp].append(pid)
        comm[pid] = c
        if c == "darlingserver":
            a = rd(f"/proc/{pid}/cmdline").split(b"\0")
            if len(a) > 1 and os.path.realpath(a[1].decode()) == os.path.realpath(PREFIX):
                root = pid
        if c == "mtlvk":
            extra.append(pid)
    pids, work = [], [root] if root else []
    while work:
        p = work.pop()
        pids.append(p)
        work += kids[p]
    return pids + extra, comm


def category(name, perm):
    if name == "":
        return "anon/" + perm
    if name == "[heap]":
        return "heap"
    if name.startswith("[stack"):
        return "stack"
    if name.startswith("[anon:"):
        inner = name[6:-1].strip()
        parts = [s.strip() for s in inner.split(":")]
        return ":".join(parts[:2]).split(" ")[0] if len(parts) > 1 else inner
    if "dyld_sim_shared_cache" in name or "/runtimes/" in name or "RuntimeRoot" in name:
        return "runtime-files"
    if ".aot." in name and name.endswith(".map"):
        return "aot.map"
    if ".aot." in name and name.endswith(".idx"):
        return "aot.idx"
    if "/dev/nvidia" in name:
        return "nvidia-dev"
    if "nvidia" in name.lower():
        return "nvidia-lib"
    if name.startswith("/memfd:"):
        return "memfd:" + re.sub(r"\s*\(deleted\)", "", name[7:])[:30]
    if name.startswith("/dev/zero"):
        return "devzero"
    if name.startswith("/dev/shm") or name.startswith("/run/user"):
        return "shm"
    if name.startswith("["):
        return name
    return "files"


def main():
    pids, comm = sim_pids()
    if not pids:
        sys.exit("no sim running for prefix " + PREFIX)
    fields = ("Pss", "SwapPss", "Private_Dirty")
    byproc, bycat, swp, pdirty = {}, collections.Counter(), collections.Counter(), collections.Counter()
    bycatproc = collections.defaultdict(collections.Counter)
    detail = collections.Counter()
    for p in pids:
        name = rd(f"/proc/{p}/cmdline").split(b"\0")[0].decode(errors="replace").split("!")[-1]
        name = os.path.basename(name) or comm.get(p, "?")
        tot = collections.Counter()
        cur = curname = None
        for line in rd(f"/proc/{p}/smaps").decode(errors="replace").splitlines():
            if re.match(r"^[0-9a-f]+-[0-9a-f]+ ", line):
                parts = line.split(None, 5)
                curname = parts[5].strip() if len(parts) > 5 else ""
                cur = category(curname, parts[1])
                continue
            k, _, v = line.partition(":")
            if k in fields and cur:
                kb = int(v.split()[0])
                tot[k] += kb
                if k == "Pss":
                    bycat[cur] += kb
                    bycatproc[cur][name] += kb
                    detail[curname[-90:]] += kb
                elif k == "SwapPss":
                    swp[cur] += kb
                else:
                    pdirty[cur] += kb
        byproc[f"{name}:{p}"] = dict(tot)
    M = 1024
    tp = sum(v.get("Pss", 0) for v in byproc.values())
    ts = sum(v.get("SwapPss", 0) for v in byproc.values())
    print(f"procs {len(byproc)}  PSS {tp / M:.0f} MB  SwapPss {ts / M:.0f} MB  PSS+Swap {(tp + ts) / M:.0f} MB")
    print("\n-- by category (PSS MB, SwapPss MB, pd = Private_Dirty MB)")
    for c, v in bycat.most_common():
        if v < 512:
            continue
        top = ", ".join(f"{n} {x / M:.0f}" for n, x in bycatproc[c].most_common(4))
        print(f"{v / M:8.0f} {swp[c] / M:6.0f} pd{pdirty[c] / M:6.0f}  {c:28s} {top}")
    print("\n-- by process (PSS MB, SwapPss MB, Private_Dirty MB)")
    for n, v in sorted(byproc.items(), key=lambda x: -x[1].get("Pss", 0))[:int(os.environ.get("TOP", "25"))]:
        print(f"{v.get('Pss', 0) / M:7.0f} {v.get('SwapPss', 0) / M:6.0f} {v.get('Private_Dirty', 0) / M:6.0f}  {n}")
    if os.environ.get("DETAIL"):
        print("\n-- top mappings by name")
        for n, v in detail.most_common(30):
            print(f"{v / M:8.0f}  {n or '[anon]'}")
    if "--json" in sys.argv:
        with open(sys.argv[sys.argv.index("--json") + 1], "w") as f:
            json.dump({"byproc": byproc, "bycat": bycat, "swap": swp, "pdirty": pdirty}, f, indent=1)


if __name__ == "__main__":
    main()
