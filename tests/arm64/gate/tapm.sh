#!/bin/bash
# tests/arm64/gate/tapm.sh TAG [N]: N (default 6) warm Settings taps from the home screen: SpringBoard CPU for 2 s
# after the tap, and frame-accurate response / content times from the simhid touch-down (simrecord open).
# The Settings icon is at (1025, 683) on an iPhone 17 (1206x2622).
. "$(dirname "$0")/lib.sh"
O=${GATE_OUT:-$SCR/gate}/$1; mkdir -p "$O"; HL=$SCR/simhid.log
SB=$(ps -eo pid,comm | awk '$2=="SpringBoard"{print $1}' | head -1)
c() { awk '{print $14+$15}' "/proc/$SB/stat"; }
for i in $(seq 1 "${2:-6}"); do
	home 6
	python3 cli/libexec/simrecord.py capture "$O/t.frames" 4 3 >/dev/null & cp=$!; sleep 1
	a=$(c); touch_ tap 1025 683; sleep 2; b=$(c); wait $cp
	t0=$(grep "simhid: touch down" "$HL" | tail -1 | awk '{print $1}')
	echo "tap $i sbcpu $(( b - a ))0ms $(python3 cli/libexec/simrecord.py open "$O/t.frames" "$t0")" | tee -a "$O/tapm.log"
done
rm -f "$O/t.frames"; home 2
