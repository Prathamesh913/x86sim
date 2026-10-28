#!/usr/bin/env python3
"""Symbolize Mach-O frames in a Linux core dump of a Darling process.

Usage: symbolize_core.py <core.maps> <addr> [addr...]

core.maps comes from the core's NT_FILE note, one mapping per line:
    start end page_offset path
"""
import functools
import re
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "cli/lib"))
import x86config  # noqa: E402

PAGE = 4096
DARLING_ROOT = x86config.get("X86SIM_PREFIX") + "/libexec/darling"
SYSTEM_ROOT = "/Volumes/SystemRoot"
FAT_MAGIC = 0xCAFEBABE
CPU_TYPE_X86_64 = 0x01000007


def linux_path(path):
    """Paths in the core are as seen inside Darling's mount namespace."""
    if path.startswith(SYSTEM_ROOT + "/"):
        return path[len(SYSTEM_ROOT):]
    # gcore (taken from outside) records the container's overlay paths, which only
    # exist inside its mount namespace; the lower layer is the install prefix
    overlay = x86config.get("X86SIM_DPREFIX")
    if path.startswith(overlay + "/") and not os.path.exists(path):
        return DARLING_ROOT + path[len(overlay):]
    if path.startswith("/home/"):
        return path
    return DARLING_ROOT + path


def load_maps(path):
    # first line may be "pagesize N" (NT_FILE page size: 4096 for kernel cores, 1 for gcore)
    maps = []
    page = PAGE
    with open(path) as f:
        for line in f:
            if line.startswith("pagesize "):
                page = int(line.split()[1])
                continue
            start, end, off, name = line.rstrip("\n").split(" ", 3)
            maps.append((int(start, 16), int(end, 16), int(off, 16) * page, linux_path(name)))
    return maps


@functools.lru_cache(None)
def slice_offset(path):
    with open(path, "rb") as f:
        hdr = f.read(8)
        if struct.unpack(">I", hdr[:4])[0] != FAT_MAGIC:
            return 0
        for i in range(struct.unpack(">I", hdr[4:])[0]):
            cputype, _, offset, _, _ = struct.unpack(">iiIII", f.read(20))
            if cputype == CPU_TYPE_X86_64:
                return offset
    return 0


@functools.lru_cache(None)
def segments(path):
    """(fileoff, filesize, vmaddr) for each x86_64 segment."""
    out = subprocess.run(["llvm-objdump", "--macho", "--arch=x86_64", "--private-headers", path],
                         capture_output=True, text=True).stdout
    segs, cur = [], {}
    for line in out.splitlines():
        m = re.match(r"\s*(vmaddr|fileoff|filesize)\s+(0x[0-9a-f]+|\d+)", line)
        if m:
            cur[m.group(1)] = int(m.group(2), 0)
            if len(cur) == 3:
                segs.append((cur["fileoff"], cur["filesize"], cur["vmaddr"]))
                cur = {}
        elif "cmd LC_SEGMENT_64" in line:
            cur = {}
    return segs


def symbolize(maps, addr):
    for start, end, off, path in maps:
        if start <= addr < end:
            if not os.path.isfile(path):
                return "? %s (not readable)" % path
            fileoff = off + (addr - start) - slice_offset(path)
            for soff, ssize, vmaddr in segments(path):
                if soff <= fileoff < soff + ssize:
                    vm = vmaddr + (fileoff - soff)
                    sym = subprocess.run(["llvm-symbolizer", "--default-arch=x86_64", "--obj=" + path, hex(vm)],
                                         capture_output=True, text=True).stdout.split("\n")[0]
                    return "%s %s+%#x (vm %#x)" % (sym, path.rsplit("/", 1)[-1], fileoff, vm)
            return "%s fileoff %#x (no segment)" % (path, fileoff)
    return "?"


def main():
    maps = load_maps(sys.argv[1])
    for a in sys.argv[2:]:
        print("%s  %s" % (a, symbolize(maps, int(a, 16))))


if __name__ == "__main__":
    main()
