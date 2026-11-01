#!/usr/bin/env python3
"""x86Code (Speed 22): arm64 devices of other phone models.

    device-a64.py models                         the models (cli/lib/devicetypes-a64.json, from Xcode 27.1)
    device-a64.py list                           var/device-a64* with model and screen
    device-a64.py create --model MODEL [NAME]    new var/device-a64-<name>: <device>/model (+ name), sim.env

A new device gets a copy (reflinks) of var/device-a64's data/ when that has booted once (apps registered, setup
done), so its first boot is a normal boot; mksimdevice.py writes the model's screen, identifiers and capabilities.
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
            print(f"{d:24} {names.get(m, m):24} {e.get('SIMULATOR_MAINSCREEN_WIDTH')}x{e.get('SIMULATOR_MAINSCREEN_HEIGHT')}"
                  f"  {e.get('SIMULATOR_DEVICE_NAME', '')}")


def cmd_create(args):
    model, name = None, None
    while args:
        a = args.pop(0)
        if a == "--model":
            model = args.pop(0)
        elif a.startswith("--model="):
            model = a.split("=", 1)[1]
        else:
            name = a
    types = models()
    if model not in types:
        sys.exit(f"device create: --model one of: {', '.join(types)}")
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
    if os.path.isdir(os.path.join(BASE, "data/var/db/lsd")):
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
               X86SIM_RUNTIME=os.environ.get("A64_RUNTIME", os.path.join(ROOT, "runtimes/ios-27.0-24A434-arm64/RuntimeRoot")),
               X86SIM_STATE=os.environ.get("A64_STATE", os.path.join(VAR, "run-a64")), X86CODE_JOBLOG="1")
    subprocess.run([sys.executable, os.path.join(ROOT, "cli/libexec/mksimdevice.py"), dev], env=env, check=True,
                   stdout=subprocess.DEVNULL)
    print(dev)


def main():
    a = sys.argv[1:] or ["list"]
    if a[0] == "models":
        for k, v in models().items():
            print(f"{k:24} {v['model']:12} {v['w']}x{v['h']} @{float(v['scale']):g}x")
    elif a[0] == "list":
        cmd_list()
    elif a[0] == "create":
        cmd_create(a[1:])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
