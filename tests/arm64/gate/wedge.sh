#!/bin/bash
# tests/arm64/gate/wedge.sh [N] (Input 2): app transitions on a running phone until SpringBoard wedges: open an app
# (openurl), HOLD s (6), home, SIGKILL the app, next app (Settings, Safari, Maps, Calendar, News). A wedge = the openurl
# request is not taken within 60 s, or the open draws no frame in 26 s. Exit 1 on a wedge, 0 after N (40) transitions.
# PHONE=<suffix> (device-a64-<suffix>, e.g. iphone-duo) picks the phone; default the main phone. KILLSPOT=1 also
# SIGKILLs Spotlight before each open (PRE s before it).
D=$(cd "$(dirname "$0")/../../.." && pwd); cd "$D"
P=${PHONE:+-$PHONE}
export SIMFB_RING_LINUX=/run/user/$(id -u)/x86sim-a64$P-simfb.ring SIMHID_SOCKET=$D/var/device-a64$P/data/simhid.sock A64_DEVICE=$D/var/device-a64$P
. tests/arm64/gate/lib.sh 2>/dev/null; DEV=$A64_DEVICE
fr() { python3 cli/libexec/fbring.py | sed -n 's/.*frame \([0-9]*\).*/\1/p'; }
apps=(Preferences:com.apple.Preferences MobileSafari:com.apple.mobilesafari Maps:com.apple.Maps MobileCal:com.apple.mobilecal News:com.apple.news)
for i in $(seq 1 "${1:-40}"); do
	a=${apps[$((i % ${#apps[@]}))]}; p=${a%%:*}; id=${a#*:}
	[ -n "$KILLSPOT" ] && for q in $(pids_of Spotlight); do kill -9 "$q"; done && sleep "${PRE:-0}"
	app "$id"; t0=$(date +%s)
	while [ -e "$DEV/data/tmp/x86sim-openurl" ] && [ $(( $(date +%s) - t0 )) -lt 60 ]; do sleep 0.5; done
	if [ -e "$DEV/data/tmp/x86sim-openurl" ]; then echo "$(date +%T) $i WEDGE: the open of $p was not taken in 60 s"; exit 1; fi
	fo=$(fr); sleep "${HOLD:-6}"
	if [ "$(fr)" = "$fo" ]; then
		sleep 20
		[ "$(fr)" = "$fo" ] && { echo "$(date +%T) $i WEDGE: the open of $p drew nothing in 26 s"; exit 1; }
	fi
	f0=$(fr); python3 cli/libexec/simtouch.py home; sleep 3; f1=$(fr)
	for q in $(pids_of "$p"); do kill -9 "$q"; done
	echo "$(date +%T) $i $p: home frames $f0 -> $f1"
	# Input 3: a home press that moved nothing is not a wedge by itself: one dropped while the app was still launching
	# leaves the phone on the home screen once the app is killed, where the second press draws nothing either (a false
	# wedge in run g40, the iPad fine a minute later). Liveness is the next open: its request must be taken in 60 s and
	# draw frames.
	[ "$f0" = "$f1" ] && echo "$(date +%T) $i note: home moved nothing (checked by the next open)"
	sleep 2
done
echo "no wedge in ${1:-40} transitions"
