#!/bin/bash
# x86Code smoke test: boot (unless a boot is running) -> home screen pixel check -> open Settings and Safari ->
# screenshot checks (framecheck.py: home = colourful wallpaper, app = light page that differs from the home screen).
#   tests/smoke.sh [out dir]      (default $X86SIM_SCRATCH/smoke-<time>; screenshots + check results there)
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT; S="$X/cli/x86sim"; FC="python3 $X/cli/libexec/framecheck.py"
O=${1:-$X86SIM_SCRATCH/smoke-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$O"
fail=0
step() { printf '%-34s %s\n' "$1" "$2"; [ "$2" = PASS ] || fail=1; }
if ps -eo args= | grep -q '[l]aunchd_sim'; then
	echo "smoke: using the running boot"
else
	"$S" boot > "$O/boot.txt" 2>&1 || { cat "$O/boot.txt"; step boot FAIL; exit 1; }
fi
"$S" dismiss > /dev/null 2>&1   # a permission prompt on the home screen (widgets)
"$S" home > /dev/null; sleep 3
if $FC wait home 120 > "$O/home-check.json"; then step "home screen" PASS; else step "home screen" FAIL; fi
$FC stats > "$O/home.json"   # reference frame for the app checks
"$S" screenshot "$O/1-home.png" 3 > /dev/null
for app in com.apple.Preferences com.apple.mobilesafari; do
	"$S" app $app > /dev/null
	if $FC wait app 60 "$O/home.json" > "$O/$app.json"; then r=PASS; else r=FAIL; fi
	sleep 3; "$S" screenshot "$O/2-$app.png" 3 > /dev/null
	step "open $app" $r
	"$S" home > /dev/null; sleep 4
	if $FC wait home 30 > /dev/null; then step "back home from $app" PASS; else step "back home from $app" FAIL; fi
done
for f in "$O"/*.png; do [ -s "$f" ] || { step "screenshot $(basename "$f")" FAIL; }; done
echo "smoke: $([ $fail = 0 ] && echo PASS || echo FAIL) (screenshots and frame stats in $O)"
exit $fail
