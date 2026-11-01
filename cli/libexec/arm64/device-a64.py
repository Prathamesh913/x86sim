#!/usr/bin/env python3
"""x86Code (Speed 22): arm64 devices of other phone models.

    device-a64.py models                         the models (cli/lib/devicetypes-a64.json, from Xcode 27.1)
    device-a64.py list                           var/device-a64* with model and screen
    device-a64.py create --model MODEL [--ios 27.x] [NAME]
                                                 new var/device-a64-<name>: <device>/model, runtime (+ name), sim.env
    device-a64.py runtimes                       the iOS versions in runtimes/ (version, build, dir)

A new device gets a copy (reflinks) of var/device-a64's data/ when that has booted once (apps registered, setup
done), so its first boot is a normal boot; mksimdevice.py writes the model's screen, identifiers and capabilities.
Speed 24: --ios picks the runtime (default 27.0); <device>/runtime names its runtimes/ dir and boot-a64.sh boots the
device on it. Only a 27.0 device gets the copy (another version's data has its own first boot).
Boot one with A64_DEVICE=var/device-a64-<name> cli/libexec/arm64/boot-a64.sh (or the launcher).
"""
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
TYPES = os.path.join(ROOT, "cli/lib/devicetypes-a64.json")
VAR = os.path.join(ROOT, "var")
BASE = os.path.join(VAR, "device-a64")


def models():
    with open(TYPES) as f:
        return json.load(f)


DEFAULT_RT = "ios-27.0-24A434-arm64"


def runtimes():
    """[(version, build, dir)] of the arm64 runtimes in runtimes/, oldest first."""
    out = []
    for d in os.listdir(os.path.join(ROOT, "runtimes")):
        m = re.match(r"ios-([0-9.]+)-([0-9A-Za-z]+)-arm64$", d)
        if m and os.path.isdir(os.path.join(ROOT, "runtimes", d, "RuntimeRoot")):
            out.append((m[1], m[2], d))
    return sorted(out, key=lambda r: [int(x) for x in r[0].split(".")])


def runtime_of(dev):
    try:
        return open(os.path.join(dev, "runtime")).read().strip() or DEFAULT_RT
    except OSError:
        return DEFAULT_RT


def env_of(dev):
    try:
        return dict(l.split("=", 1) for l in open(os.path.join(dev, "sim.env")).read().splitlines() if "=" in l)
    except OSError:
        return {}


def cmd_list():
    names = {v["model"]: k for k, v in models().items()}
    for d in sorted(os.listdir(VAR)):
        p = os.path.join(VAR, d)
        if d.startswith("device-a64") and os.path.exists(os.path.join(p, "sim.env")):
            e = env_of(p)
            m = e.get("SIMULATOR_MODEL_IDENTIFIER", "")
            rt = re.match(r"ios-([0-9.]+)-", runtime_of(p))
            print(f"{d:24} {names.get(m, m):24} {e.get('SIMULATOR_MAINSCREEN_WIDTH')}x{e.get('SIMULATOR_MAINSCREEN_HEIGHT')}"
                  f"  iOS {rt[1] if rt else '?':5} {e.get('SIMULATOR_DEVICE_NAME', '')}")


def cmd_create(args):
    model, name, ios = None, None, None
    while args:
        a = args.pop(0)
        if a in ("--model", "--ios"):
            v = args.pop(0)
            if a == "--model":
                model = v
            else:
                ios = v
        elif a.startswith("--model="):
            model = a.split("=", 1)[1]
        elif a.startswith("--ios="):
            ios = a.split("=", 1)[1]
        else:
            name = a
    types = models()
    if model not in types:
        sys.exit(f"device create: --model one of: {', '.join(types)}")
    rts = runtimes()
    rt = DEFAULT_RT
    if ios:
        hit = [r for r in rts if ios in (r[0], r[1], r[2])]
        if not hit:
            sys.exit(f"device create: --ios one of: {', '.join(r[0] for r in rts)}")
        rt = hit[0][2]
    name = name or model
    slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
    dev = os.path.join(VAR, "device-a64-" + slug)
    if os.path.exists(dev):
        sys.exit(f"device create: {dev} exists")
    os.makedirs(dev)
    with open(os.path.join(dev, "model"), "w") as f:
        f.write(model + "\n")
    with open(os.path.join(dev, "name"), "w") as f:
        f.write(name + "\n")
    with open(os.path.join(dev, "runtime"), "w") as f:
        f.write(rt + "\n")
    if rt == runtime_of(BASE) and os.path.isdir(os.path.join(BASE, "data/var/db/lsd")):
        subprocess.run(["cp", "-a", "--reflink=auto", os.path.join(BASE, "data"), os.path.join(dev, "data")], check=True)
        for x in ("var/log", "var/run", "tmp"):
            shutil.rmtree(os.path.join(dev, "data", x), ignore_errors=True)
            os.makedirs(os.path.join(dev, "data", x))
        for x in ("simhid.sock", "framebuffer.bgra"):
            try:
                os.unlink(os.path.join(dev, "data", x))
            except OSError:
                pass
    env = dict(os.environ, X86SIM_ARCH="arm64", X86SIM_DEVICE=dev,
               X86SIM_RUNTIME=os.path.join(ROOT, "runtimes", rt, "RuntimeRoot"),
               X86SIM_STATE=os.environ.get("A64_STATE", os.path.join(VAR, "run-a64")), X86CODE_JOBLOG="1")
    subprocess.run([sys.executable, os.path.join(ROOT, "cli/libexec/mksimdevice.py"), dev], env=env, check=True,
                   stdout=subprocess.DEVNULL)
    print(dev)


def main():
    a = sys.argv[1:] or ["list"]
    if a[0] == "models":
        for k, v in models().items():
            print(f"{k:24} {v['model']:12} {v['w']}x{v['h']} @{float(v['scale']):g}x")
    elif a[0] == "runtimes":
        for v, b, d in runtimes():
            print(f"{v:6} {b:10} {d}")
    elif a[0] == "list":
        cmd_list()
    elif a[0] == "create":
        cmd_create(a[1:])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
