#!/bin/bash
# tests/arm64/gate/perfopen.sh TAG (Speed 28): iTLB / L1i counters. SpringBoard idle 5 s, then Preferences during a
# cold open (killed first; perf attaches as soon as the process appears, 6 s). Output: $SCR/gate/TAG/perf.log
. "$(dirname "$0")/lib.sh"
O=${GATE_OUT:-$SCR/gate}/$1; mkdir -p "$O"; L=$O/perf.log
EV=cycles,instructions,bp_l1_tlb_miss_l2_tlb_hit,bp_l1_tlb_miss_l2_tlb_miss.all,bp_l1_tlb_miss_l2_tlb_miss.if2m,ic_tag_hit_miss.instruction_cache_miss
pid_of() { ps -eo pid,comm | awk -v c="$1" '$2==c{print $1}' | head -1; }
home 4
echo "== SpringBoard idle 5 s" >>"$L"
perf stat -x, -e $EV -p "$(pid_of SpringBoard)" -- sleep 5 2>>"$L"
for r in 1 2; do
	for p in $(ps -eo pid,comm | awk '$2=="Preferences"{print $1}'); do kill -9 "$p"; done
	home 6
	touch_ tap 1020 668 &
	for i in $(seq 1 100); do p=$(pid_of Preferences); [ -n "$p" ] && break; sleep 0.05; done
	echo "== Preferences cold $r (pid $p)" >>"$L"
	perf stat -x, -e $EV -p "$p" -- sleep 6 2>>"$L"
	wait
done
home 4
cat "$L"
