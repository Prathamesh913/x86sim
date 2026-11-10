"""x86Sim: the same configuration as cli/lib/config.sh, for the Python tools (it runs config.sh and reads the result)."""
import os
import subprocess

ROOT = os.environ.get("X86SIM_ROOT") or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_cfg = None


def load():
    global _cfg
    if _cfg is None:
        env = dict(os.environ, X86SIM_ROOT=ROOT)
        out = subprocess.run(["sh", "-c", '. "$1" >/dev/null 2>&1; env -0', "sh", os.path.join(ROOT, "cli/lib/config.sh")],
                             env=env, capture_output=True, check=True).stdout
        _cfg = dict(kv.split("=", 1) for kv in out.decode().split("\0") if "=" in kv)
    return _cfg


def get(key):
    return load()[key]


def device():
    return get("X86SIM_DEVICE")


def data():
    return os.path.join(device(), "data")
