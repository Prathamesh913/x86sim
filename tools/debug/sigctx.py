#!/usr/bin/env python3
"""Find the register state saved by a Linux signal frame in a Darling core.

Darling handles SIGILL/SIGSEGV on an alternate stack and re-raises, so the faulting
thread's own registers only survive in the ucontext on that stack. This scans every
thread's stack for the ucontext's csgsfs word (0x002b000000000033) and prints the
general registers before it, the symbolized rip, and the words at rsp.
    tools/debug/sigctx.py <core> [words]
"""
import os, re, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "cli/lib"))
import x86config  # noqa: E402

X = x86config.ROOT
MLDR = x86config.get("X86SIM_PREFIX") + "/libexec/darling/usr/libexec/darling/mldr"
NAMES = "R8 R9 R10 R11 R12 R13 R14 R15 RDI RSI RBP RBX RDX RAX RCX RSP RIP EFL".split()

def gdb(core, *cmds):
    args = ["gdb", "-batch"] + sum((["-ex", c] for c in cmds), []) + [MLDR, core]
    return subprocess.run(args, capture_output=True, text=True).stdout

def words_at(core, thread, expr, n):
    out = gdb(core, "thread %d" % thread, "x/%dgx %s" % (n, expr))
    words = []
    for l in out.splitlines():
        m = re.match(r"(0x[0-9a-f]+).*?:\s+(.*)", l)
        if m:
            a = int(m.group(1), 16)
            for i, v in enumerate(m.group(2).split()):
                words.append((a + 8 * i, int(v, 16)))
    return words

def sym(maps, *addrs):
    return subprocess.run(["python3", "-I", X + "/tools/debug/symbolize_core.py", maps] + [hex(a) for a in addrs],
                          capture_output=True, text=True).stdout

def main():
    core, n = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 4000
    maps = x86config.get("X86SIM_SCRATCH") + "/sigctx.maps"  # x86Code: not /tmp (small tmpfs)
    with open(maps, "w") as f:
        f.write(subprocess.run(["sh", "-c", "readelf -n '%s' | awk '/Page size:/{print \"pagesize\", $3} /NT_FILE/{f=1} f && /^ +0x/ {s=$1; e=$2; o=$3; getline p; gsub(/^ +/,\"\",p); print s, e, o, p}'" % core],
                               capture_output=True, text=True).stdout)
    threads = len(re.findall(r"^\*? +\d+ +", gdb(core, "info threads"), re.M))
    for t in range(1, threads + 1):
        words = words_at(core, t, "$rsp", n)
        for i, (a, v) in enumerate(words):
            if v == 0x2b000000000033 and i >= 18:
                regs = {name: words[i - 18 + k][1] for k, name in enumerate(NAMES)}
                print("== thread %d: signal frame at %#x" % (t, a))
                print("  " + " ".join("%s=%#x" % (k, regs[k]) for k in NAMES))
                print("  rip: " + sym(maps, regs["RIP"]).strip())
                stack = [v for _, v in words_at(core, t, "%#x" % regs["RSP"], 400) if v > 0x100000000]
                for l in sym(maps, *stack).splitlines():
                    if " ?" not in l and "no segment" not in l:
                        print("   ", l.split(None, 1)[1][:110])
                break

main()
