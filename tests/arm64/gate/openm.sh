#!/bin/bash
# tests/arm64/gate/openm.sh TAG [APPS...] (Speed 25): frame-timed cold and warm opens by tapping the home-screen
# icon. Cold = the app's process is killed first (SIGKILL on the host pid) and the open starts from home; warm = home,
# then tap again (app suspended). All times are from the simhid touch-down, read from the frame ring timestamps
# (simrecord capture, scale 4): response = first changed frame, black = the launch gap (frames >95% black), content =
# first frame of the settled screen (simrecord open). Default apps: Settings Messages Health Wallet Maps.
# iPhone 17 (1206x2622), 27.2 home with the Shortcuts widget top-left (Nov 2026 layout: Settings 3rd row right).
# Speed 31: x86ui / Cadence (installed with x86sim install) open by bundle id; t0 = the trigger write.
# Env: SECS (capture length, default 16), N (cold+warm rounds per app, default 1).
. "$(dirname "$0")/lib.sh"
O=${GATE_OUT:-$SCR/gate}/$1; shift; mkdir -p "$O"; HL=$SCR/simhid.log
declare -A XY=([Settings]="1020 967" [Messages]="744 2444" [Health]="188 967" [Wallet]="464 967" [Maps]="744 668"
	[x86ui]="id com.x86sim.test.x86ui" [Cadence]="id com.designgears.alarm" [Calendar]="744 368" [Photos]="1020 368" [News]="1020 668" [Safari]="468 2444")
declare -A PROC=([Settings]=Preferences [Messages]=MobileSMS [Health]=Health [Wallet]=Passbook [Maps]=Maps
	[Calendar]=MobileCal [Photos]=MobileSlideShow [News]=News [Safari]=MobileSafari [x86ui]=x86ui [Cadence]=Cadence)
APPS=("$@"); [ ${#APPS[@]} = 0 ] && APPS=(Settings Messages Health Wallet Maps)
# RAM 2: labelled icons are found on the home screen (lib.sh icon_xy); the table is the fallback (and the dock)
home 4; for a in "${APPS[@]}"; do case ${XY[$a]} in id*) ;; *) XY[$a]=$(icon_xy "$a" "${XY[$a]}") ;; esac; done
measure() { # LABEL X Y
	python3 cli/libexec/simrecord.py capture "$O/m.frames" "${SECS:-16}" 4 >/dev/null & local cp=$!; sleep 0.8
	local t0
	if [ "$2" = id ]; then t0=$(date +%s%N); app "$3"; wait $cp  # Speed 31: by bundle id (openurl trigger)
	else touch_ tap $2 $3; wait $cp; t0=$(grep "simhid: touch down" "$HL" | tail -1 | awk '{print $1}'); fi
	python3 - "$O/m.frames" "$t0" "$1" <<'PY' | tee -a "$O/openm.log"
import json, subprocess, sys, os
sys.path.insert(0, "cli/libexec"); import simrecord, numpy as np
fr = simrecord.read_frames(sys.argv[1]); t0 = int(sys.argv[2])
b0 = b1 = None
for t, px in fr:
    if t <= t0: continue
    dark = (px.max(axis=2) < 30).mean() > 0.95
    if dark and b0 is None: b0 = t
    if b0 is not None and not dark and b1 is None: b1 = t
m = simrecord.open_metrics([(t / 1e9, simrecord.small(px.astype(np.float32))) for t, px in fr], t0 / 1e9)
ms = lambda t: None if t is None else round((t - t0) / 1e6)
print(f"{sys.argv[3]:<16} resp {m.get('response_s')} s  black {ms(b0)}..{ms(b1)} ms  content {m.get('content_s')} s  last {ms(fr[-1][0])} ms")
PY
	rm -f "$O/m.frames"
}
for r in $(seq 1 "${N:-1}"); do
	for a in "${APPS[@]}"; do
		for p in $(pids_of "${PROC[$a]}"); do kill -9 "$p"; done
		home 6; measure "$a-cold" ${XY[$a]}
		shot "$O/$a-cold.png"
		home 6; measure "$a-warm" ${XY[$a]}
		home 4
	done
done
