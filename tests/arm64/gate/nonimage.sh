#!/bin/bash
# tests/arm64/gate/nonimage.sh TAG APP [SECS] (Speed 31): where a cold open's CPU goes. Kills APP's process (openm.sh
# names: Settings, x86ui, Cadence, ...), opens it, perf-records the new process for SECS (default 6) and splits the
# samples: image (the AOT .map), live (anonymous JIT code = translated live, the non-image share), libarm64jit,
# other. Output: $SCR/gate/TAG/nonimage.log
. "$(dirname "$0")/lib.sh"
O=${GATE_OUT:-$SCR/gate}/$1; mkdir -p "$O"; L=$O/nonimage.log; A=$2; S=${3:-6}
declare -A XY=([Settings]="1020 967" [Maps]="744 668" [Health]="188 967" [x86ui]="id com.x86sim.test.x86ui" [Cadence]="id com.designgears.alarm")
declare -A PROC=([Settings]=Preferences [Maps]=Maps [Health]=Health [x86ui]=x86ui [Cadence]=Cadence)
for p in $(pids_of "${PROC[$A]}"); do kill -9 "$p"; done
home 6
case ${XY[$A]} in id*) ;; *) XY[$A]=$(icon_xy "$A" "${XY[$A]}") ;; esac  # RAM 2: lib.sh icon_xy
set -- ${XY[$A]}
if [ "$1" = id ]; then app "$2"; else touch_ tap $1 $2 & fi
for i in $(seq 1 200); do p=$(pid_of "${PROC[$A]}"); [ -n "$p" ] && break; sleep 0.025; done
perf record -q -F 2000 -o "$O/$A.perf" -p "$p" -- sleep "$S" 2>/dev/null
wait
perf report -i "$O/$A.perf" --sort dso -q --stdio 2>/dev/null | awk -v a="$A" '
	{ pct=$1; sub("%","",pct); d=$2
	  if (d ~ /\.map$|\.aot/) im+=pct; else if (d ~ /anon|unknown|perf-|JIT|jit/ && d !~ /libarm64jit/) lv+=pct;
	  else if (d ~ /libarm64jit/) lj+=pct; else ot+=pct }
	END { printf "%s cold: image %.1f%%  live %.1f%%  libarm64jit %.1f%%  other %.1f%%\n", a, im, lv, lj, ot }' | tee -a "$L"
perf report -i "$O/$A.perf" --sort dso -q --stdio 2>/dev/null | head -8 >> "$L"
home 3
