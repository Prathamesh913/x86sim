#!/usr/bin/env python3
"""x86Code (Speed 22): iOS simulator runtimes straight from Apple, no Mac and no Apple ID.

    runtime-fetch.py list                 iOS 27.x runtimes in Apple's catalogue (the one Xcode reads)
    runtime-fetch.py url BUILD            the runtime's download URL (from gdmf.apple.com, as Xcode asks for it)
    runtime-fetch.py fetch BUILD          download into runtimes/ios-<ver>-<build>-arm64/, decrypt, unpack, RuntimeRoot

How Xcode gets them: the catalogue (devimages-cdn .../index2.dvtdownloadableindex) lists each runtime as a MobileAsset
download ("downloadMethod": "mobileAsset", no authentication). mobileassetd asks gdmf.apple.com/v2/assets for asset
type com.apple.MobileAsset.iOSSimulatorRuntime with the Xcode simulator audience and the build it wants
(RequestedBuild); the answer (a JWT) has an updates.cdn-apple.com URL to an encrypted Apple Archive (AEA) and its
ArchiveDecryptionKey. gdmf's certificate chains to Apple's own root CA (fetched from apple.com/appleca).
Unpacking: `ipsw fw aea -b KEY` decrypts the archive to the cryptex disk image, `7z x -snld` reads the APFS image
(FINDINGS: Linux-side extraction). Runtimes are never committed (runtimes/ is gitignored).
"""
import base64
import json
import os
import plistlib
import subprocess
import sys
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
INDEX = "https://devimages-cdn.apple.com/downloads/xcode/simulators/index2.dvtdownloadableindex"
GDMF = "https://gdmf.apple.com/v2/assets"
APPLE_ROOT = "https://www.apple.com/appleca/AppleIncRootCertificate.cer"
XCODE_SIM_AUDIENCE = "02d8e57e-dd1c-4090-aa50-b4ed2aef0062"


def catalogue():
    with urllib.request.urlopen(INDEX) as r:
        d = plistlib.loads(r.read())
    out = []
    for x in d.get("downloadables", []):
        v = x.get("simulatorVersion", {})
        if x.get("platform") == "com.apple.platform.iphoneos" and v.get("version", "").startswith("27"):
            out.append((v["version"], v["buildUpdate"], x.get("name", ""), x.get("fileSize", 0), x.get("architectures")))
    return out


def apple_ca(tmp):
    pem = os.path.join(tmp, "apple-root.pem")
    der = os.path.join(tmp, "apple-root.cer")
    urllib.request.urlretrieve(APPLE_ROOT, der)
    subprocess.run(["openssl", "x509", "-inform", "der", "-in", der, "-out", pem], check=True)
    return pem


def asset(build):
    version = next((c[0] for c in catalogue() if c[1] == build), None)
    if not version:
        sys.exit(f"runtime-fetch: build {build} is not in Apple's catalogue (see `list`)")
    body = {"AssetAudience": XCODE_SIM_AUDIENCE, "AssetType": "com.apple.MobileAsset.iOSSimulatorRuntime",
            "ClientVersion": 2, "CertIssuanceDay": "2023-12-10", "RequestedBuild": build, "Build": build,
            "SimulatorVersion": version, "ProductVersion": version, "BuildVersion": build}
    with tempfile.TemporaryDirectory() as tmp:
        r = subprocess.run(["curl", "-sf", "--cacert", apple_ca(tmp), "-H", "Content-Type: application/json", "-X", "POST",
                            "--data", json.dumps(body), GDMF], capture_output=True, text=True, check=True).stdout
    p = r.strip().split(".")
    d = json.loads(base64.urlsafe_b64decode(p[1] + "=" * (-len(p[1]) % 4)))
    for a in d.get("Assets", []):
        if a.get("Build") == build:
            return version, a
    sys.exit(f"runtime-fetch: gdmf has no asset for {build}: {d.get('Message', d)}")


def fetch(build):
    version, a = asset(build)
    dst = os.path.join(ROOT, "runtimes", f"ios-{version}-{build}-arm64")
    dl = os.path.join(dst, "download")
    os.makedirs(dl, exist_ok=True)
    aar = os.path.join(dl, "runtime.aar")
    url = a["__BaseURL"] + a["__RelativePath"]
    print(f"iOS {version} ({build}): {a['_DownloadSize'] / 1e9:.1f} GB from {url}", flush=True)
    subprocess.run(["curl", "-f", "-C", "-", "-o", aar, url], check=True)
    if os.path.getsize(aar) != a["_DownloadSize"]:
        sys.exit("runtime-fetch: size mismatch")
    out = os.path.join(dst, "decrypted")
    os.makedirs(out, exist_ok=True)
    subprocess.run(["ipsw", "fw", "aea", aar, "-b", a["ArchiveDecryptionKey"], "-o", out], check=True)
    dmgs = [os.path.join(dp, f) for dp, _, fs in os.walk(out) for f in fs if f.endswith(".dmg")]
    if not dmgs:
        sys.exit(f"runtime-fetch: no disk image in {out}")
    dmg = max(dmgs, key=os.path.getsize)
    subprocess.run(["7z", "x", "-snld", "-y", "-o" + os.path.join(dst, "extracted"), dmg], stdout=subprocess.DEVNULL)
    rr = os.path.join("extracted/Library/Developer/CoreSimulator/Profiles/Runtimes", f"iOS {version}.simruntime",
                      "Contents/Resources/RuntimeRoot")
    if not os.path.isdir(os.path.join(dst, rr)):
        sys.exit(f"runtime-fetch: no {rr} after unpacking")
    # 7z refuses a link that goes through another link (FINDINGS); the one known case
    lw = os.path.join(dst, rr, "usr/lib/swift/libswiftWebKit.dylib")
    if not os.path.lexists(lw):
        os.symlink("../../../System/Library/Frameworks/WebKit.framework/WebKit", lw)
    link = os.path.join(dst, "RuntimeRoot")
    if not os.path.lexists(link):
        os.symlink(rr, link)
    print(dst)


def main():
    a = sys.argv[1:] or ["list"]
    if a[0] == "list":
        for v, b, name, size, arch in catalogue():
            have = os.path.isdir(os.path.join(ROOT, "runtimes", f"ios-{v}-{b}-arm64", "RuntimeRoot"))
            mark = "  (downloaded)" if have else ""
            print(f"{b:12} {name:42} {size / 1e9:4.1f} GB{mark}")
    elif a[0] == "url" and len(a) > 1:
        v, x = asset(a[1])
        print(x["__BaseURL"] + x["__RelativePath"])
    elif a[0] == "fetch" and len(a) > 1:
        fetch(a[1])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
