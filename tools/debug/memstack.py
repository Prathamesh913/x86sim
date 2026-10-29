#!/usr/bin/env python3
"""Memory of the running simulator stack (the x86sim-*.scope that runs rundev.sh): the scope's anon + shmem and swap
(cgroup memory.stat / memory.swap.current), the process count, and per-process Pss / SwapPss / Anonymous / Private_Dirty /
Shared_Clean from /proc/<pid>/smaps_rollup. Needs read access to the processes (same user).
  memstack.py [--json out.json] [--top N]
"""
import glob
import json
import os
import sys


def scope_cgroup():
    for d in glob.glob("/sys/fs/cgroup/user.slice/user-%d.slice/user@%d.service/app.slice/x86sim-*.scope" %
                       (os.getuid(), os.getuid())):
        try:
            for pid in open(d + "/cgroup.procs").read().split():
                if b"rundev.sh" in open(f"/proc/{pid}/cmdline", "rb").read():
                    return d
        except OSError:
            pass
    sys.exit("no x86sim scope running rundev.sh")


def rollup(pid):
    out = {}
    try:
        for line in open(f"/proc/{pid}/smaps_rollup"):
            parts = line.split()
            if len(parts) == 3 and parts[2] == "kB":
                out[parts[0].rstrip(":")] = int(parts[1])
        name = open(f"/proc/{pid}/cmdline", "rb").read().split(b"\0")[0].decode(errors="replace")
    except OSError:
        return None
    out["name"] = os.path.basename(name.split("!")[-1]) or "?"
    return out


def main():
    args = sys.argv[1:]
    top = int(args[args.index("--top") + 1]) if "--top" in args else 15
    cg = scope_cgroup()
    stat = dict(line.split() for line in open(cg + "/memory.stat"))
    swap = int(open(cg + "/memory.swap.current").read())
    procs = []
    for pid in open(cg + "/cgroup.procs").read().split():
        r = rollup(pid)
        if r:
            r["pid"] = int(pid)
            procs.append(r)
    g = 1 << 30
    tot = {k: sum(p.get(k, 0) for p in procs) / (1 << 20) for k in ("Pss", "SwapPss", "Anonymous", "Private_Dirty", "Shared_Clean")}
    summary = {"anon_gb": int(stat["anon"]) / g, "shmem_gb": int(stat["shmem"]) / g,
               "anon_plus_shmem_gb": (int(stat["anon"]) + int(stat["shmem"])) / g, "file_gb": int(stat["file"]) / g,
               "swap_gb": swap / g, "procs": len(procs), "total_gb": (int(stat["anon"]) + int(stat["shmem"]) + swap) / g,
               "sum_gb": dict(tot)}
    print(json.dumps(summary, indent=1))
    print("%8s %8s %8s %8s %8s  %s" % ("Pss MB", "SwPss MB", "Anon MB", "PrivD MB", "ShrC MB", "process"))
    for p in sorted(procs, key=lambda p: -p.get("Pss", 0) - p.get("SwapPss", 0))[:top]:
        print("%8d %8d %8d %8d %8d  %s" % (p.get("Pss", 0) >> 10, p.get("SwapPss", 0) >> 10, p.get("Anonymous", 0) >> 10,
                                       p.get("Private_Dirty", 0) >> 10, p.get("Shared_Clean", 0) >> 10, p["name"]))
    if "--json" in args:
        json.dump({"summary": summary, "procs": procs}, open(args[args.index("--json") + 1], "w"), indent=1)


if __name__ == "__main__":
    main()
