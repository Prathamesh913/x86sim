#!/usr/bin/env python3
"""tests/arm64/gate/maptiles.py PNG: did Maps draw map tiles? (Speed 27)
Looks at the map area above the search card (device pixels y 200-1450 on a 1206x2622 iPhone 17 frame) and counts
water-blue and land-green pixels and distinct colours. A rendered map (globe or street level) has both; the states
Maps showed while it couldn't load tiles have neither: a bare globe (cream sphere with its grid) or a flat cream
area. Prints one line "tiles yes|no blue B green G colours C" and exits 0 for yes, 1 for no."""
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
box = (0, int(h * 200 / 2622), w, int(h * 1450 / 2622))
px = list(im.crop(box).resize((302, 312)).getdata())
n = len(px)
blue = sum(1 for r, g, b in px if b > r + 40 and b > g + 5) / n
green = sum(1 for r, g, b in px if g > r + 25 and g > b + 15) / n
cols = len({(r // 16, g // 16, b // 16) for r, g, b in px})
ok = blue + green >= 0.15 and cols >= 150
print(f"tiles {'yes' if ok else 'no'} blue {blue:.2f} green {green:.2f} colours {cols}")
sys.exit(0 if ok else 1)
