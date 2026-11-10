#!/usr/bin/env python3
"""x86Sim: rewrite a copied device's absolute data/ paths to its own (device-a64.py create; repair of old clones).

    device-repath.py DEVICE [OLD_DEVICE]     DEVICE's data/ was copied from OLD_DEVICE (default var/device-a64)
                                             (refuses a booted DEVICE; --force: anyway)

The simulator sees a device's data as /Volumes/SystemRoot<repo>/var/<device>/data and many stores keep that path
absolute. A copy of var/device-a64's data/ kept the base phone's: lsd's csstore held every app's data container as
.../var/device-a64/data/Containers/..., so apps on a clone ran with HOME in the base phone's containers (Safari on
ram2 wrote the main phone's databases). containermanagerd and MobileInstallation keep no absolute paths (they
resolve containers under HOME at runtime), so the rest are caches and preferences:
  - plists (binary/XML, also bplists nested as data, e.g. NSKeyedArchiver blobs, FrontBoard's kvs values): rewritten
    through plistlib, strings re-encoded with their new lengths;
  - sqlite dbs (FrontBoard applicationState.db, nsurlsessiond, MediaLibrary): opened (the WAL is checkpointed in),
    text cells replaced, blob cells that are bplists rewritten as above;
  - JSON and other text: replaced; symlinks with an absolute target (MobileAsset's *_latest.locker): relinked;
  - lsd's csstores (CSStore2: bplists inside length-prefixed units, no rewrite possible): deleted; lsd rebuilds the
    store at boot from the runtime's apps and MobileInstallation's registrations with this device's paths;
  - CoreSpotlight indexes and e5rt bundle caches: deleted (rebuilt);
  - anything else that still has the old path is reported (exit 1).
Only DEVICE's files are written (a reflinked file is copied on write; the base phone is never touched).
"""
import os
import plistlib
import shutil
import sqlite3
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


def sim_path(dev):
    return "/Volumes/SystemRoot" + os.path.realpath(dev) + "/"


class Repath:
    def __init__(self, old, new):
        self.old, self.new = old, new
        self.ob, self.nb = old.encode(), new.encode()
        self.oldk = old.rstrip("/")  # the device dir itself (".../var/device-a64" with no slash)

    def s(self, v):
        if v.startswith(self.oldk) and v == self.oldk:
            return self.new.rstrip("/")
        return v.replace(self.old, self.new)

    def obj(self, v):
        if isinstance(v, str):
            return self.s(v)
        if isinstance(v, bytes):
            if self.ob not in v and self.oldk.encode() not in v:
                return v
            if v.startswith(b"bplist00") or v.startswith(b"<?xml"):
                return self.plist_bytes(v)
            return v
        if isinstance(v, dict):
            return {self.s(k) if isinstance(k, str) else k: self.obj(x) for k, x in v.items()}
        if isinstance(v, list):
            return [self.obj(x) for x in v]
        if isinstance(v, plistlib.UID):
            return v
        return v

    def plist_bytes(self, b):
        try:
            p = plistlib.loads(b)
        except Exception:
            return b
        fmt = plistlib.FMT_BINARY if b.startswith(b"bplist") else plistlib.FMT_XML
        return plistlib.dumps(self.obj(p), fmt=fmt, sort_keys=False)


def sqlite_fix(r, db):
    c = sqlite3.connect(db)
    n = 0
    for (t,) in c.execute("select name from sqlite_master where type='table'").fetchall():
        cols = [x[1] for x in c.execute(f"pragma table_info(\"{t}\")")]
        for col in cols:
            rows = c.execute(f"select rowid, \"{col}\" from \"{t}\"").fetchall() if cols else []
            for rid, v in rows:
                if isinstance(v, str) and r.old in v:
                    nv = r.s(v)
                elif isinstance(v, bytes) and r.ob in v:
                    nv = r.obj(v)
                else:
                    continue
                if nv != v:
                    c.execute(f"update \"{t}\" set \"{col}\"=? where rowid=?", (nv, rid))
                    n += 1
    c.commit()
    c.execute("vacuum")  # freed pages still held old rows' bytes
    c.execute("pragma wal_checkpoint(truncate)")
    c.close()
    return n


def has(path, ob):
    try:
        with open(path, "rb") as f:
            return ob in f.read()
    except OSError:
        return False


def booted(dev):
    """a process of the phone runs: its launchd_sim's command line names <dev>/data/ (as boot-a64.sh's stop_prefix)"""
    key = (os.path.realpath(dev) + "/data/").encode()
    for p in os.listdir("/proc"):
        if p.isdigit():
            try:
                with open(f"/proc/{p}/cmdline", "rb") as f:
                    if key in f.read():
                        return True
            except OSError:
                pass
    return False


def main():
    args = [a for a in sys.argv[1:] if a != "--force"]
    if not args:
        sys.exit(__doc__)
    dev = args[0]
    base = args[1] if len(args) > 1 else os.path.join(ROOT, "var/device-a64")
    if os.path.realpath(dev) == os.path.realpath(base):
        sys.exit("device-repath: DEVICE is the base device")
    if booted(dev) and "--force" not in sys.argv:
        sys.exit(f"device-repath: {dev} is booted (stop it first: A64_STOP_ONLY=1 boot-a64.sh with its A64_STATE/A64_DPREFIX)")
    r = Repath(sim_path(base), sim_path(dev))
    data = os.path.join(dev, "data")
    hits = []
    for d, dirs, files in os.walk(data):
        for f in files:
            p = os.path.join(d, f)
            if os.path.islink(p):
                t = os.readlink(p)
                if r.old in t:  # MobileAsset's atomic_instance_latest.locker -> <data>/.../atomic_instance_<id>.locker
                    os.unlink(p)
                    os.symlink(r.s(t), p)
                    print(f"  relinked {os.path.relpath(p, data)}")
            elif os.path.isfile(p) and has(p, r.ob):
                hits.append(p)
    gone, bad = set(), []
    dbs = set()
    for p in hits:
        rel = os.path.relpath(p, data)
        if any(p.startswith(g + "/") for g in gone):
            continue
        if rel.startswith("var/db/lsd/") and p.endswith(".csstore"):
            os.unlink(p)
            print(f"  deleted {rel} (lsd rebuilds it)")
            continue
        cut = None
        for key in ("/index.spotlightV2/", "/CoreSpotlight/Priority/", "/com.apple.e5rt.e5bundlecache/"):
            if key in p:
                cut = p[: p.index(key) + len(key) - 1]
        if cut:
            shutil.rmtree(cut, ignore_errors=True)
            gone.add(cut)
            print(f"  deleted {os.path.relpath(cut, data)}/ (cache)")
            continue
        for sfx in ("-wal", "-shm", "-journal"):
            if p.endswith(sfx) and os.path.exists(p[: -len(sfx)]):
                p = p[: -len(sfx)]
        with open(p, "rb") as f:
            head = f.read(16)
        if head.startswith(b"SQLite format 3"):
            dbs.add(p)
            continue
        with open(p, "rb") as f:
            b = f.read()
        if head.startswith(b"bplist00") or head.startswith(b"<?xml"):
            nb = r.plist_bytes(b)
            if r.ob in nb:  # not a parseable plist (e.g. a launchd plist with odd content): text
                nb = nb.replace(r.ob, r.nb)
        else:
            try:
                nb = b.decode("utf-8").replace(r.old, r.new).encode()
            except UnicodeDecodeError:
                bad.append(rel)
                continue
        tmp = p + ".repath"
        with open(tmp, "wb") as f:
            f.write(nb)
        shutil.copystat(p, tmp)
        os.replace(tmp, p)
        print(f"  rewrote {rel}")
    for db in sorted(dbs):
        n = sqlite_fix(r, db)
        print(f"  rewrote {os.path.relpath(db, data)} ({n} cells)")
    for p in hits:
        if os.path.exists(p) and has(p, r.ob) and os.path.relpath(p, data) not in bad:
            bad.append(os.path.relpath(p, data))
    for b in bad:
        print(f"  STILL HAS {r.old}: {b}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
