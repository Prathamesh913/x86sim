#!/bin/bash
# tests/arm64/gate/simcost.sh TAG [SECS] (Speed 32): what the booted phone costs at idle. Over SECS (default 10) at the
# home screen: CPU of the whole sim (its systemd scope's cpu.stat, darlingserver included), darlingserver's own CPU,
# SpringBoard's CPU and its context switches (all threads, voluntary + involuntary) and CPU per main-thread wakeup.
# Also: `simcost.sh TAG cmd...` runs cmd and prints the same deltas around it (e.g. a tap or an open).
. "$(dirname "$0")/lib.sh"
O=${GATE_OUT:-$SCR/gate}/$1; mkdir -p "$O"; shift
DS=$(ps -eo pid=,args= | awk -v d="$(realpath "$DEV")" '$2=="darlingserver"{print $1}' | while read -r p; do
	of_phone "$p" 2>/dev/null && echo "$p"; done | head -1)
[ -n "$DS" ] || DS=$(ps -eo pid=,comm= | awk '$2=="darlingserver"{print $1}' | head -1)
SCOPE=/sys/fs/cgroup$(sed -n 's/^0:://p' "/proc/$DS/cgroup")
SB=$(pid_of SpringBoard)
cpu_scope() { awk '$1=="usage_usec"{print $2}' "$SCOPE/cpu.stat"; }
cpu_pid() { awk '{print ($14+$15)*10000}' "/proc/$1/stat"; } # usec
csw_pid() { local s=0 t; for t in /proc/$1/task/*/status; do s=$(( s + $(awk '/ctxt_switches/{n+=$2} END{print n+0}' "$t") )); done; echo $s; }
csw_main() { awk '/ctxt_switches/{n+=$2} END{print n+0}' "/proc/$1/status"; }
snap() { echo "$(cpu_scope) $(cpu_pid "$DS") $(cpu_pid "$SB") $(csw_pid "$SB") $(csw_main "$SB")"; }
a=($(snap)); t0=$(date +%s.%N)
if [ $# -gt 0 ] && ! [[ $1 =~ ^[0-9]+$ ]]; then "$@"; else sleep "${1:-10}"; fi
b=($(snap)); t1=$(date +%s.%N)
python3 - "$t0" "$t1" "${a[@]}" "${b[@]}" <<'E' | tee -a "$O/simcost.log"
import sys
t0, t1 = float(sys.argv[1]), float(sys.argv[2]); a = list(map(int, sys.argv[3:8])); b = list(map(int, sys.argv[8:13]))
d = [y - x for x, y in zip(a, b)]; s = t1 - t0
print(f"{s:.1f}s: sim {d[0]/1e3/s:.0f} ms/s ({d[0]/1e4/s:.1f}% of a CPU), darlingserver {d[1]/1e3/s:.0f} ms/s, "
      f"SpringBoard {d[2]/1e3/s:.0f} ms/s, {d[3]/s:.0f} csw/s (main {d[4]/s:.0f}/s), "
      f"{(d[2]/d[4] if d[4] else 0):.0f} us SpringBoard CPU per main-thread switch")
E
