#!/bin/bash
# x86Code apps test: install the sample apps (tests/apps, built on the Mac with build/mac-build-apps.sh) with
# `x86sim install`, launch each with `x86sim launch` and check that it draws: a pixel of the app's own colour at a
# known spot, and for the animated ones (SwiftUI, Metal, SpriteKit) that two frames a second apart differ.
# Then uninstall and install one again (the icon comes back) and open it by tapping its icon.
#   tests/apps.sh [out dir]      (default $X86SIM_SCRATCH/apps-<time>); needs a running boot (x86sim boot)
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT; S="$X/cli/x86sim"; B="$X/tests/apps/build"
O=${1:-$X86SIM_SCRATCH/apps-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$O"
fail=0
step() { printf '%-40s %s\n' "$1" "$2"; [ "$2" = PASS ] || fail=1; }
ps -eo args= | grep -q '[l]aunchd_sim' || { echo "apps: no boot running (x86sim boot)"; exit 1; }
[ -d "$B/HelloUIKit.app" ] || { echo "apps: build the apps first: build/mac-build-apps.sh"; exit 1; }
# check <png> <x> <y> <r,g,b> <tolerance>: the pixel (in a 3x scaled-down screenshot) is near the colour
check() { python3 -I -c '
import sys; from PIL import Image
p = Image.open(sys.argv[1]).convert("RGB").getpixel((int(sys.argv[2]), int(sys.argv[3])))
want = [int(v) for v in sys.argv[4].split(",")]
print(p); sys.exit(0 if max(abs(a - b) for a, b in zip(p, want)) <= int(sys.argv[5]) else 1)' "$@"; }
differ() { python3 -I -c '
import sys; from PIL import Image, ImageChops
a, b = (Image.open(f).convert("RGB") for f in sys.argv[1:3])
box = ImageChops.difference(a, b).getbbox(); print(box); sys.exit(0 if box else 1)' "$@"; }
# name bundle-id  x y colour tolerance  animated
apps="HelloUIKit com.x86code.hellouikit 196 200 0,195,208 30 0
SwiftUIDemo com.x86code.swiftuidemo 30 120 242,242,247 10 1
MetalDemo com.x86code.metaldemo 380 800 - - 1
SpriteDemo com.x86code.spritedemo 30 300 17,17,17 12 1"
"$S" home > /dev/null; sleep 2
while read -r name bid x y col tol anim; do
	"$S" uninstall "$bid" > /dev/null 2>&1
	if "$S" install "$B/$name.app" > "$O/install-$name.txt" 2>&1; then step "install $name" PASS; else step "install $name" FAIL; continue; fi
	if "$S" launch "$bid" > "$O/launch-$name.txt" 2>&1; then step "launch $name" PASS; else step "launch $name" FAIL; fi
	sleep 8
	"$S" screenshot "$O/$name-1.png" 3 > /dev/null; sleep 1; "$S" screenshot "$O/$name-2.png" 3 > /dev/null
	if [ "$col" = - ] || check "$O/$name-2.png" $x $y $col $tol >> "$O/checks.txt"; then step "$name draws" PASS; else step "$name draws" FAIL; fi
	if [ "$anim" = 1 ]; then
		if differ "$O/$name-1.png" "$O/$name-2.png" >> "$O/checks.txt"; then step "$name animates" PASS; else step "$name animates" FAIL; fi
	fi
	"$S" home > /dev/null; sleep 2
done <<< "$apps"
# reinstall: the icon is back on the home screen and opens the app
"$S" uninstall com.x86code.hellouikit > /dev/null 2>&1; sleep 2
"$S" install "$B/HelloUIKit.app" > /dev/null 2>&1; sleep 5
"$S" screenshot "$O/home.png" 3 > /dev/null
# its icon: the largest patch of the icon's teal (0,145,147) on the home screen
pos=$(python3 -I -c '
import sys; from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); w, h = im.size
pts = [(x, y) for y in range(0, h, 2) for x in range(0, w, 2) if max(abs(a - b) for a, b in zip(im.getpixel((x, y)), (0, 145, 147))) < 12]
if len(pts) > 200: print(3 * sum(p[0] for p in pts) // len(pts), 3 * sum(p[1] for p in pts) // len(pts))' "$O/home.png")
echo "HelloUIKit icon at: ${pos:-not found}" >> "$O/checks.txt"
[ -n "$pos" ] && "$S" tap $pos; sleep 5; "$S" screenshot "$O/tap-HelloUIKit.png" 3 > /dev/null
if check "$O/tap-HelloUIKit.png" 196 200 0,195,208 30 >> "$O/checks.txt"; then step "reinstall + tap icon opens HelloUIKit" PASS; else step "reinstall + tap icon opens HelloUIKit" FAIL; fi
"$S" home > /dev/null
echo "apps: $([ $fail = 0 ] && echo PASS || echo FAIL) (screenshots in $O)"
exit $fail
