#!/usr/bin/env python3
"""x86Code: pre-answer the permission prompts on the device, as a test device would be set up.

    cli/libexec/seedperms.py <device dir> [bundle id ...]     (default: the apps tools/robust tests open)

Run before the boot (cli/libexec/rundev.sh does, after mksimdevice.py); the daemons read these files when they start:
- TCC (tccd, data/Library/TCC/TCC.db): kTCCServiceAll and the usual per-service rows, allowed (auth_value 2), as
  `xcrun simctl privacy <dev> grant all <bundle id>` writes them.
- Location (locationd, data/Library/Caches/locationd/clients.plist): "Authorization" 2 per app, which is what
  `simctl privacy grant location` leaves there.
- Notifications (SpringBoard's BulletinBoard, data/Library/BulletinBoard/VersionedSectionInfo.plist): a section per
  app with authorizationStatus 2 (authorized) and banner alerts. There is no simctl command for this; each section
  is a copy of an authorized one (the Files section SpringBoard writes, or a built-in fallback) with the bundle id
  and name replaced.
Existing answers are kept (an app the user denied stays denied).
"""
import os, plistlib, sqlite3, sys

APPS = {
    "com.apple.mobilesafari": "Safari", "com.apple.news": "News", "com.apple.Preview": "Preview",
    "com.apple.DocumentsApp": "Files", "com.apple.Preferences": "Settings", "com.apple.Maps": "Maps",
    "com.apple.Health": "Health", "com.apple.mobilecal": "Calendar", "com.apple.Fitness": "Fitness",
    "com.apple.Passbook": "Wallet", "com.apple.MobileSMS": "Messages", "com.apple.MobileAddressBook": "Contacts",
    "com.apple.Passwords": "Passwords", "com.apple.reminders": "Reminders", "com.apple.mobileslideshow": "Photos",
    "com.apple.shortcuts": "Shortcuts",
}
TCC_SERVICES = ["kTCCServiceAll", "kTCCServicePhotos", "kTCCServicePhotosAdd", "kTCCServiceAddressBook",
                "kTCCServiceCalendar", "kTCCServiceReminders", "kTCCServiceCamera", "kTCCServiceMicrophone",
                "kTCCServiceMediaLibrary", "kTCCServiceMotion", "kTCCServiceSpeechRecognition",
                "kTCCServiceBluetoothAlways", "kTCCServiceUserTracking", "kTCCServiceFocusStatus",
                "kTCCServiceWillow"]
# the access table as tccd 26.5 creates it (from a Mac's simulator), for a device whose tccd hasn't run yet
TCC_SCHEMA = """CREATE TABLE IF NOT EXISTS access (service TEXT NOT NULL, client TEXT NOT NULL, client_type INTEGER NOT NULL,
 auth_value INTEGER NOT NULL, auth_reason INTEGER NOT NULL, auth_version INTEGER NOT NULL, csreq BLOB, policy_id INTEGER,
 indirect_object_identifier_type INTEGER, indirect_object_identifier TEXT NOT NULL DEFAULT 'UNUSED',
 indirect_object_code_identity BLOB, flags INTEGER,
 last_modified INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)), pid INTEGER, pid_version INTEGER,
 boot_uuid TEXT NOT NULL DEFAULT 'UNUSED',
 last_reminded INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)),
 PRIMARY KEY (service, client, client_type, indirect_object_identifier))"""


def seed_tcc(lib, apps):
    os.makedirs(lib + "/TCC", exist_ok=True)
    db = sqlite3.connect(lib + "/TCC/TCC.db")
    have = db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='access'").fetchone()
    if not have:
        db.execute(TCC_SCHEMA)
    n = 0
    for b in apps:
        for s in TCC_SERVICES:
            # auth_reason 2 = user consent, as if the prompt had been answered; INSERT OR IGNORE keeps earlier answers
            c = db.execute("INSERT OR IGNORE INTO access (service, client, client_type, auth_value, auth_reason, "
                           "auth_version, flags) VALUES (?, ?, 0, 2, 2, 1, 0)", (s, b))
            n += c.rowcount
    db.commit()
    db.close()
    return n


def seed_location(lib, apps):
    p = lib + "/Caches/locationd/clients.plist"
    os.makedirs(os.path.dirname(p), exist_ok=True)
    d = plistlib.load(open(p, "rb")) if os.path.exists(p) else {}
    n = 0
    for b in apps:
        e = d.setdefault("i%s:" % b, {"BundleId": b})
        if not isinstance(e, dict) or "Authorization" in e:
            continue
        e["Authorization"] = 2
        n += 1
    with open(p + ".new", "wb") as f:
        plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
    os.rename(p + ".new", p)
    return n


def section_template(sections):
    # an authorized section: SpringBoard's own Files section if it has one, else the same, as a Mac's simulator wrote it
    t = sections.get("com.apple.DocumentsApp")
    if t:
        return t, "com.apple.DocumentsApp", "Files"
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "seedperms-section.plist")
    return open(here, "rb").read(), "com.apple.DocumentsApp", "Files"


def seed_notifications(lib, apps):
    p = lib + "/BulletinBoard/VersionedSectionInfo.plist"
    os.makedirs(os.path.dirname(p), exist_ok=True)
    d = plistlib.load(open(p, "rb")) if os.path.exists(p) else {"sectionInfo": {}, "sectionInfoVersionNumber": 2}
    sections = d.setdefault("sectionInfo", {})
    tmpl, tid, tname = section_template(sections)
    n = 0
    for b, name in apps.items():
        if b in sections:
            continue
        a = plistlib.loads(tmpl)
        a["$objects"] = [b if o == tid else name if o == tname else o for o in a["$objects"]]
        objs = a["$objects"]
        top = objs[a["$top"]["root"].data]
        st = objs[top["sectionInfoSettings"].data]
        st["authorizationStatus"] = 2
        sections[b] = plistlib.dumps(a, fmt=plistlib.FMT_BINARY)
        n += 1
    with open(p + ".new", "wb") as f:
        plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
    os.rename(p + ".new", p)
    return n


def main():
    lib = os.path.realpath(sys.argv[1]) + "/data/Library"
    apps = {b: APPS.get(b, b.split(".")[-1]) for b in sys.argv[2:]} or APPS
    t = seed_tcc(lib, apps)
    l = seed_location(lib, apps)
    n = seed_notifications(lib, apps)
    print("seedperms: %d TCC rows, %d location, %d notification sections added" % (t, l, n))


main()
