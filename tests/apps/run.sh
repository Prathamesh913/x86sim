#!/bin/bash
# x86Code: the test-app regression suite. Builds tests/apps/x86* on the Mac (build.sh), installs them on a running phone
# (`x86sim install`, as simctl install), launches each and checks it logs-first:
#   1. the install answer, 2. the app's results file (Documents/x86test-results.txt in its data container: every check
#   "X86TEST <app> <check> PASS|FAIL"), 3. launchd_sim's log for the app exiting / crashing, 4. new crash reports
#   (<data>/Library/Logs/DiagnosticReports), 5. a screenshot (fb2png) per app for the visual check, saved in OUT.
# Usage: tests/apps/run.sh <device> [--no-build] [app...]   (device: a phone's name / folder / dir, as `x86sim install`)
# Env: SECS=per-app wait for "done" (default 90), OUT=screenshot dir (default $X86SIM_SCRATCH/apps/run-<time>).
set -u
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
. "$X86SIM_ROOT/cli/lib/config.sh"
. "$X86SIM_ROOT/cli/lib/pickdevice.sh"
X=$X86SIM_ROOT; A=$X/tests/apps
dev=${1:?usage: tests/apps/run.sh <device> [--no-build] [app...]}; shift
pick_device "$dev" || { echo "run.sh: no device $dev" >&2; exit 2; }
export X86SIM_DEVICE
build=1; [ "${1:-}" = --no-build ] && { build=0; shift; }
apps=${*:-$(cd "$A" && ls -d x86*/ | tr -d /)}
SECS=${SECS:-90}
OUT=${OUT:-$X86SIM_SCRATCH/apps/run-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$OUT"
D=$X86SIM_DEVICE/data
ST=$(grep -l "^$X86SIM_DEVICE\$" "$X"/var/run-a64*/device 2>/dev/null | head -1); ST=${ST%/device}
LLOG=${ST:+$ST/launchd.log}
# the phone's frame ring (sim.env SIMFB_RING, a Darling path), else the default one
RING=$(sed -n 's#^SIMFB_RING=/Volumes/SystemRoot##p' "$X86SIM_DEVICE/sim.env"); RING=${RING:-ring}
[ "$RING" != ring ] && export SIMFB_RING_LINUX=$RING   # simtouch (prompts) reads the same ring
[ "$build" = 1 ] && { nice "$A/build.sh" $apps || exit 1; }
fail=0
crashes() { find "$D/Library/Logs/DiagnosticReports" -name '*.ips' -newer "$OUT/.t0" 2>/dev/null; }
for n in $apps; do
	app=$X86SIM_SCRATCH/apps/$n.app; bid=com.x86code.test.$n
	echo "== $n"
	touch "$OUT/.t0"; l0=$( [ -n "$LLOG" ] && wc -l < "$LLOG" || echo 0)
	"$X/cli/x86sim" install "$X86SIM_DEVICE" "$app" >"$OUT/$n.install" 2>&1 || { echo "  install FAILED: $(tail -1 "$OUT/$n.install")"; fail=1; continue; }
	echo "  installed"
	"$X/cli/x86sim" launch "$X86SIM_DEVICE" "$bid" >"$OUT/$n.launch" 2>&1 || { echo "  launch FAILED: $(tail -1 "$OUT/$n.launch")"; fail=1; continue; }
	res=""
	for i in $(seq "$SECS"); do
		sleep 1
		# permission prompts (notifications, ...) are answered as a tester would: simtouch dismiss taps Allow (OCR)
		[ $((i % 10)) = 0 ] && timeout 30 python3 "$X/cli/libexec/simtouch.py" dismiss 1 >>"$OUT/$n.prompts" 2>&1
		res=$(ls -t "$D"/Containers/Data/Application/*/Documents/x86test-results.txt 2>/dev/null | while read -r f; do
			grep -q "^X86TEST $n " "$f" && { echo "$f"; break; }; done)
		[ -n "$res" ] && grep -q "^X86TEST $n done" "$res" && break
	done
	[ -n "$res" ] && cp "$res" "$OUT/$n.results" && sed 's/^/  /' "$res" || { echo "  no results file"; fail=1; }
	[ -n "$res" ] && grep -q "^X86TEST $n done" "$res" || { echo "  did not finish in $SECS s"; fail=1; }
	[ -n "$res" ] && grep -q " FAIL " "$res" && fail=1
	if [ -n "$LLOG" ]; then
		tail -n +$((l0 + 1)) "$LLOG" | grep -E "UIKitApplication:$bid.*(exited|crashed|signal|abnormal)" | sed 's/^/  launchd: /' | tee "$OUT/$n.launchd"
		[ -s "$OUT/$n.launchd" ] && fail=1
	fi
	c=$(crashes | grep -i "$n"); [ -n "$c" ] && { echo "  CRASH: $c"; cp $c "$OUT/" ; fail=1; }
	sleep "${SETTLE:-6}"   # web views and animations paint a few seconds after the app reports done
	python3 "$X/cli/libexec/fb2png.py" "$RING" "$OUT/$n.png" 2 >/dev/null 2>&1 && echo "  screen: $OUT/$n.png"
done
echo "results in $OUT"; [ $fail = 0 ] && echo PASS || echo FAIL
exit $fail
