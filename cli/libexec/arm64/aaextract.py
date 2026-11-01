#!/usr/bin/env python3
"""x86Code (Speed 22): extract an uncompressed Apple Archive ("AA01" headers, what a decrypted simulator-runtime AEA
holds) on Linux: regular files and directories only (enough for the runtime's cryptex disk image).

    aaextract.py ARCHIVE list
    aaextract.py ARCHIVE extract OUTDIR

Each entry: "AA01", u16 header size, then fields: 3-char key + 1-char type. Types: '1','2','4','8' unsigned ints of
that size; '*' flag (no value); 'P' u16 length + string; 'T' timespec (8+4 bytes); 'S' 8+4; 'A','B','C' a blob whose
size is a u16/u32/u64 here and whose bytes follow the header in field order; 'F' u16 field size + bytes ('H' hashes:
type-dependent, see HASH). TYP1 is the entry type: 'F' file, 'D' directory, 'L' link, 'M' metadata.
"""
import lzma
import os
import struct
import sys

HASH = {"F": 4, "G": 20, "H": 32, "I": 48, "J": 64}  # hash fields by type char: CKS (crc32) and SH1/SH2/SH3/SH5


def entries(f):
    while True:
        hd = f.read(6)
        if len(hd) < 6:
            return
        if hd[:4] != b"AA01":
            raise ValueError(f"bad header magic {hd[:4]!r} at {f.tell() - 6}")
        size = struct.unpack("<H", hd[4:])[0]
        h = f.read(size - 6)
        fields, blobs, o = {}, [], 0
        while o < len(h):
            key, t = h[o:o + 3].decode(), chr(h[o + 3]); o += 4
            if t in "1248":
                n = int(t); fields[key] = int.from_bytes(h[o:o + n], "little"); o += n
            elif t == "*":
                fields[key] = True
            elif t == "P":
                n = struct.unpack_from("<H", h, o)[0]; fields[key] = h[o + 2:o + 2 + n].decode(errors="replace"); o += 2 + n
            elif t in "TS":
                o += 12
            elif t in "ABC":
                n = {"A": 2, "B": 4, "C": 8}[t]; blobs.append((key, int.from_bytes(h[o:o + n], "little"))); o += n
            elif t in HASH:
                o += HASH[t]
            else:
                raise ValueError(f"unknown field type {key}{t}")
        yield fields, blobs


class Pbzx:
    """Forward-only reader over a pbzx stream (magic, u64 BE chunk size, then per chunk: u64 BE uncompressed size,
    u64 BE stored size, xz data or raw bytes when stored == uncompressed) of `total` bytes in f."""
    def __init__(self, f, total):
        self.f, self.end, self.buf, self.pos = f, f.tell() + total, b"", 0
        if f.read(4) != b"pbzx":
            raise ValueError("not pbzx")
        f.read(8)

    def _fill(self):
        if self.f.tell() >= self.end:
            return False
        us, cs = struct.unpack(">QQ", self.f.read(16))
        d = self.f.read(cs)
        self.buf = self.buf[self.pos:] + (lzma.decompress(d) if cs < us else d)
        self.pos = 0
        return True

    def read(self, n):
        while len(self.buf) - self.pos < n and self._fill():
            pass
        b = self.buf[self.pos:self.pos + n]
        self.pos += len(b)
        return b

    def seek(self, n, whence=1):
        while n:
            n -= len(self.read(min(n, 1 << 24)))

    def tell(self):
        return -1


def main():
    path, cmd = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else None
    with open(path, "rb") as f:
        walk(f, cmd, out)


def walk(f, cmd, out):
    for fields, blobs in entries(f):
        typ, name = chr(fields.get("TYP", 0)), fields.get("PAT", "")
        if cmd == "list":
            print(typ, name, [(k, s) for k, s in blobs])
        for key, size in blobs:
            if typ == "M" and key == "DAT" and size > 1 << 20:
                # a decrypted runtime AEA: metadata entries around one pbzx stream holding the real archive
                start = f.tell()
                walk(Pbzx(f, size), cmd, out)
                f.seek(start + size)
                continue
            if cmd == "extract" and typ == "F" and key == "DAT":
                dst = os.path.join(out, name)
                if os.path.relpath(dst, out).startswith(".."):
                    raise ValueError(f"unsafe path {name!r}")
                os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
                with open(dst, "wb") as w:
                    left = size
                    while left:
                        b = f.read(min(left, 1 << 24))
                        if not b:
                            raise ValueError("truncated archive")
                        w.write(b); left -= len(b)
                print(dst, size)
            else:
                f.seek(size, 1)
        if cmd == "extract" and typ == "D" and name:
            os.makedirs(os.path.join(out, name), exist_ok=True)


if __name__ == "__main__":
    main()
