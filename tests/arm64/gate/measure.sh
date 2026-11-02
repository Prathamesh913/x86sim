#!/bin/bash
# tests/arm64/gate/measure.sh TAG (Speed 35): the round's standard A/B on a booted phone. Fresh first open of
# Settings (run right after the boot), perf counters on cold Preferences (perfopen.sh), killed cold + warm opens
# (openm.sh, N=2: Settings x86ui Cadence), 5 warm taps (tapm.sh). Summary lines in $SCR/gate/TAG/summary.txt.
D=$(dirname "$0"); . "$D/lib.sh"
rm -f "${GATE_OUT:-$SCR/gate}/$1/perf/perf.log"
T=$1; O=${GATE_OUT:-$SCR/gate}/$T; mkdir -p "$O"; mark_logs
N=1 "$D/openm.sh" "$T/fresh" Settings > "$O/fresh.txt" 2>&1
"$D/perfopen.sh" "$T/perf" > /dev/null 2>&1
N=2 "$D/openm.sh" "$T/open" Settings x86ui Cadence > "$O/open.txt" 2>&1
"$D/tapm.sh" "$T/tap" 5 > "$O/tap.txt" 2>&1
{ echo "== fresh"; tail -4 "$O/fresh.txt"; echo "== open"; tail -14 "$O/open.txt"; echo "== taps"; tail -6 "$O/tap.txt"
  echo "== perf"; grep -E "==|instructions|cache_miss|l2_tlb_miss.all" "$O/perf/perf.log"; echo "crashes: $(crashes)"; } > "$O/summary.txt"
cat "$O/summary.txt"
