#!/usr/bin/env python3
# tests/arm64/gate/switcherlook.py PNG (Speed 35): a looser visual check for the app switcher, used by lib.sh
# app_switcher when OCR of the full frame misses the card label (s34h3: the "Settings" label at y~325 was not read).
# Prints "label <word>" when OCR of the label band (y 150-600, cropped and upscaled 2x) finds an app name, else
# "card" when the frame has the switcher's shape: the left strip (x 0-150, y 900-1800) differs from the card body
# (x 400-1000, same rows) by more than 40 grey levels on average (a full-screen app or the home screen is uniform
# there); else "none". iPhone 17 frame (1206x2622).
import subprocess, sys, tempfile
from PIL import Image, ImageStat
APPS = {"Settings", "Photos", "Maps", "Messages", "News", "Safari", "Wallet", "Health", "Files", "Calendar"}
im = Image.open(sys.argv[1]).convert("L")
band = im.crop((0, 150, im.size[0], 600)).resize((im.size[0] * 2, 900))
with tempfile.NamedTemporaryFile(suffix=".png") as t:
    band.save(t.name)
    out = subprocess.run(["tesseract", t.name, "stdout", "--psm", "11"], capture_output=True, text=True, timeout=30).stdout
for w in out.split():
    w = w.strip(".,:;'\"")
    if w in APPS:
        print("label", w)
        sys.exit(0)
left = ImageStat.Stat(im.crop((0, 900, 150, 1800))).mean[0]
body = ImageStat.Stat(im.crop((400, 900, 1000, 1800))).mean[0]
print("card" if abs(left - body) > 40 else "none", f"left {left:.0f} body {body:.0f}")
