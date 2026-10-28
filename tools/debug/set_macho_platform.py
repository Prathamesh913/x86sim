#!/usr/bin/env python3
"""Rewrite the platform in a thin x86_64 Mach-O's LC_BUILD_VERSION.

Usage: set_macho_platform.py <file> [platform [minos]]
Defaults: platform 7 (iOS simulator), minos 14.0.

dyld_sim refuses to load macOS-platform images that are not the host's
/usr/lib/system libraries, so Darling-built helpers injected into simulator
processes are retagged.
"""
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_BUILD_VERSION = 0x32
LC_VERSION_MIN_MACOSX = 0x24


def main():
    path = sys.argv[1]
    platform = int(sys.argv[2]) if len(sys.argv) > 2 else 7
    major, minor = (sys.argv[3] if len(sys.argv) > 3 else "14.0").split(".")
    minos = (int(major) << 16) | (int(minor) << 8)

    with open(path, "r+b") as f:
        data = bytearray(f.read())
        magic, _, _, _, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", data, 0)
        if magic != MH_MAGIC_64:
            sys.exit("%s: not a thin 64-bit Mach-O" % path)
        off, done = 32, False
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<II", data, off)
            if cmd == LC_BUILD_VERSION:
                struct.pack_into("<III", data, off + 8, platform, minos, minos)
                done = True
            elif cmd == LC_VERSION_MIN_MACOSX:
                sys.exit("%s: uses LC_VERSION_MIN_MACOSX, not handled" % path)
            off += cmdsize
        if not done:
            sys.exit("%s: no LC_BUILD_VERSION" % path)
        f.seek(0)
        f.write(data)
    print("%s: platform %d, minos %s.%s" % (path, platform, major, minor))


if __name__ == "__main__":
    main()
