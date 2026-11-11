#!/usr/bin/env python3
"""x86Sim: the macOS SDK from Apple's public software-update CDN, without a Mac, Xcode or an Apple ID.

Apple serves the Command Line Tools packages the way `xcode-select --install` fetches them: a public catalog on
swscan.apple.com lists them and swcdn.apple.com serves them without authentication. The SDK is its own package
(CLTools_macOSNMOS_SDK.pkg, ~70 MB): a xar archive whose Payload is pbzx (chunked xz) around a cpio (odc) tree. This
reads all three formats itself and keeps only Library/Developer/CommandLineTools/SDKs/MacOSX<version>.sdk.

The headers are the ones build/sdk/sim-sdk.py puts in the substitute iPhoneSimulator.sdk; the SDK itself is
sdks/MacOSX.sdk for simhost (build/linux-env.sh). Using it means accepting Apple's macOS SDK and Xcode agreement:
nothing it downloads is committed or redistributed.

Usage: build/sdk/macos-sdk.py [--out sdks/MacOSX.sdk] [--catalog URL] [--list] [--force]"""
import argparse, gzip, hashlib, io, lzma, os, plistlib, shutil, struct, sys, tempfile, urllib.request, zlib
import xml.etree.ElementTree as ET

ROOT = os.path.realpath(os.path.join(os.path.dirname(__file__), "../.."))
CATALOG = ("https://swscan.apple.com/content/catalogs/others/index-27-26-15-14-13-12-10.16-10.15-10.14-10.13-10.12-"
           "10.11-10.10-10.9-mountainlion-lion-snowleopard-leopard.merged-1.sucatalog.gz")
SDK_PKG = "CLTools_macOSNMOS_SDK.pkg"  # the newest SDK of a CLT release (LMOS: the previous macOS's)


def get(url):
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def releases(catalog):
    """[(post date, product id, sdk package url, size)] of the Command Line Tools releases, oldest first."""
    data = get(catalog)
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    out = []
    for pid, p in plistlib.loads(data).get("Products", {}).items():
        for pk in p.get("Packages", []):
            if pk.get("URL", "").endswith("/" + SDK_PKG):
                out.append((p.get("PostDate"), pid, pk["URL"], pk.get("Size", 0)))
    return sorted(out, key=lambda r: (str(r[0]), r[1]))


def download(url, size, dst):
    with urllib.request.urlopen(url, timeout=60) as r, open(dst, "wb") as f:
        n = 0
        while chunk := r.read(1 << 20):
            f.write(chunk); n += len(chunk)
            if size:
                print(f"\r  {n >> 20}/{size >> 20} MB", end="", file=sys.stderr)
    print(file=sys.stderr)
    if size and n != size:
        sys.exit(f"macos-sdk: {url}: got {n} bytes, the catalog says {size}")


def xar_payload(f):
    """(offset, length, sha1) of the xar's Payload in the heap."""
    magic, hsize, _, toc_c, _toc_u = struct.unpack(">4sHHQQ", f.read(24))
    if magic != b"xar!":
        sys.exit("macos-sdk: not a xar package")
    f.seek(hsize)
    toc = ET.fromstring(zlib.decompress(f.read(toc_c)))
    heap = hsize + toc_c
    for fe in toc.iter("file"):
        if fe.findtext("name") == "Payload":
            d = fe.find("data")
            sha = d.find("archived-checksum")
            return heap + int(d.findtext("offset")), int(d.findtext("length")), (sha.get("style"), sha.text.strip())
    sys.exit("macos-sdk: no Payload in the package")


class PbzxReader(io.RawIOBase):
    """The pbzx stream as a file: 16-byte chunk headers (flags, length), each chunk xz or stored."""

    def __init__(self, f, length):
        self.f, self.left, self.buf = f, length, b""
        if f.read(4) != b"pbzx":
            sys.exit("macos-sdk: the Payload is not pbzx")
        f.read(8); self.left -= 12

    def readable(self):
        return True

    def readinto(self, b):
        while not self.buf and self.left > 0:
            _, n = struct.unpack(">QQ", self.f.read(16)); c = self.f.read(n); self.left -= 16 + n
            self.buf = lzma.decompress(c) if c[:6] == b"\xfd7zXZ\x00" else c
        n = min(len(b), len(self.buf)); b[:n] = self.buf[:n]; self.buf = self.buf[n:]
        return n


def extract_sdk(stream, dst_parent):
    """Walks the odc cpio; writes the entries under .../SDKs/ to dst_parent. Returns the SDK dir names."""
    def read(n):
        b = stream.read(n)
        while len(b) < n:
            more = stream.read(n - len(b))
            if not more:
                sys.exit("macos-sdk: truncated payload")
            b += more
        return b

    marker, names, links = "/CommandLineTools/SDKs/", set(), {}
    while True:
        h = read(76)
        if h[:6] != b"070707":
            sys.exit("macos-sdk: unexpected cpio format")
        dev, ino, mode = int(h[6:12], 8), int(h[12:18], 8), int(h[18:24], 8)
        nlink, namesize, filesize = int(h[36:42], 8), int(h[59:65], 8), int(h[65:76], 8)
        name = read(namesize)[:-1].decode()
        if name == "TRAILER!!!":
            break
        data = read(filesize)
        i = name.find(marker)
        if i < 0:
            continue
        rel = name[i + len(marker):]
        if not rel:
            continue
        names.add(rel.split("/")[0])
        p = os.path.join(dst_parent, rel)
        kind = mode & 0o170000
        if kind == 0o040000:
            os.makedirs(p, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if kind == 0o120000:
            os.symlink(data.decode(), p)
        elif kind == 0o100000:
            if nlink > 1 and not data and (dev, ino) in links:
                os.link(links[(dev, ino)], p)
                continue
            with open(p, "wb") as f:
                f.write(data)
            os.chmod(p, mode & 0o777 | 0o200)
            if nlink > 1:
                links[(dev, ino)] = p
    return sorted(names)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=os.path.join(ROOT, "sdks/MacOSX.sdk"))
    ap.add_argument("--catalog", default=CATALOG)
    ap.add_argument("--list", action="store_true", help="list the Command Line Tools releases and exit")
    ap.add_argument("--force", action="store_true", help="replace an existing --out")
    a = ap.parse_args()
    rel = releases(a.catalog)
    if not rel:
        sys.exit("macos-sdk: no Command Line Tools SDK package in the catalog")
    if a.list:
        for date, pid, url, size in rel:
            print(date, pid, size >> 20, "MB", url)
        return
    if os.path.lexists(a.out) and not a.force:
        print(f"macos-sdk: {a.out} exists (--force replaces it)", file=sys.stderr)
        return
    date, pid, url, size = rel[-1]
    print(f"macos-sdk: Command Line Tools {pid} ({date}): {url}", file=sys.stderr)
    out = os.path.abspath(a.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.dirname(out), prefix=".macos-sdk.") as tmp:
        pkg = os.path.join(tmp, SDK_PKG)
        download(url, size, pkg)
        with open(pkg, "rb") as f:
            off, length, (alg, digest) = xar_payload(f)
            if alg in ("sha1", "sha256", "sha512"):
                f.seek(off); h = hashlib.new(alg)
                for left in iter(lambda: min(1 << 20, off + length - f.tell()), 0):
                    h.update(f.read(left))
                if h.hexdigest() != digest:
                    sys.exit("macos-sdk: the Payload checksum does not match the package's table of contents")
            f.seek(off)
            sdks = extract_sdk(io.BufferedReader(PbzxReader(f, length), 1 << 20), os.path.join(tmp, "SDKs"))
        real = [s for s in sdks if os.path.isdir(os.path.join(tmp, "SDKs", s)) and s != "MacOSX.sdk"]
        if not real:
            sys.exit(f"macos-sdk: no SDK in the package ({sdks})")
        best = max(real, key=lambda s: [int(x) for x in s[6:-4].split(".") if x.isdigit()])
        if os.path.lexists(out):
            shutil.rmtree(out) if os.path.isdir(out) and not os.path.islink(out) else os.unlink(out)
        os.rename(os.path.join(tmp, "SDKs", best), out)
    print(f"macos-sdk: {best} -> {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
