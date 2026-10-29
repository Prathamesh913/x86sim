#!/bin/bash
# x86Code stability soak: with a running boot, for <minutes> (default 120) open apps in turn, tap/scroll in them, go
# home, and every <interval> s record the memory (RSS) of darlingserver, mtlvk, backboardd, SpringBoard and the
# number of crashes (Linux core dumps of Darling processes, launchd_sim's "exited due to" signal lines). Each round
# checks that the app drew and the home screen came back (framecheck.py).
#   tests/soak.sh [minutes] [out dir]    (out: $X86SIM_SCRATCH/soak-<time>: samples.csv, rounds.log, summary.txt)
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT; S="$X/cli/x86sim"; FC="python3 $X/cli/libexec/framecheck.py"
MIN=${1:-120}; O=${2:-$X86SIM_SCRATCH/soak-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$O"
ps -eo args= | grep -q '[l]aunchd_sim' || { echo "soak: no boot running (x86sim boot)"; exit 1; }
APPS=${SOAK_APPS:-"com.apple.Preferences com.apple.mobilesafari com.apple.Maps com.apple.mobileslideshow com.apple.Health com.x86code.devtools com.apple.news com.apple.DocumentsApp"}
UDID=$(sed -n 's/^SIMULATOR_UDID=//p' "$X86SIM_DEVICE/sim.env")
LL="$X86SIM_DPREFIX/private/var/tmp/com.apple.CoreSimulator.SimDevice.$UDID/launchd.log"
since=$(date '+%Y-%m-%d %H:%M:%S'); t0=$(date +%s)
exits0=$(cat "$LL" "$LL".[0-9] 2>/dev/null | grep -c 'exited due to SIG\(SEGV\|ABRT\|ILL\|BUS\|TRAP\)')

# RSS in MiB of the first process whose argv matches (Darling processes are mldr with the program in argv)
rss() { ps -eo rss=,args= | awk -v p="$1" 'index($0, p) && !/awk/ {printf "%.0f", $1 / 1024; exit}'; }
cores() { coredumpctl list --since "$since" --no-legend 2>/dev/null | grep -c -E 'mldr|darlingserver'; }
crashes() { echo $(( $(cat "$LL" "$LL".[0-9] 2>/dev/null | grep -c 'exited due to SIG\(SEGV\|ABRT\|ILL\|BUS\|TRAP\)') - exits0 )); }
sample() {
	echo "$(( $(date +%s) - t0 )),$(rss bin/darlingserver),$(ps -o rss= -C mtlvk | awk '{s+=$1} END {printf "%.0f", s/1024}'),$(rss /usr/libexec/backboardd),$(rss CoreServices/SpringBoard.app/SpringBoard),$(ps -eo rss=,args= | awk '/[m]ldr|RuntimeRoot/ {s+=$1} END {printf "%.0f", s/1024}'),$(cores),$(crashes),$rounds,$bad" >> "$O/samples.csv"
}
echo "secs,darlingserver_mb,mtlvk_mb,backboardd_mb,springboard_mb,all_darling_mb,cores,crash_exits,rounds,failed_rounds" > "$O/samples.csv"
rounds=0; bad=0; last=0
set -- $APPS; napps=$#
"$S" home > /dev/null; sleep 2
$FC stats > "$O/home.json" 2>/dev/null
sample
while [ $(( $(date +%s) - t0 )) -lt $(( MIN * 60 )) ]; do
	app=$(echo $APPS | cut -d' ' -f$(( rounds % napps + 1 )))
	rounds=$((rounds + 1)); ok=1
	"$S" app "$app" > /dev/null 2>&1
	$FC wait app 40 "$O/home.json" > /dev/null 2>&1 || ok=0
	# drawn page or not (dark apps fail the light-page check): interact anyway
	"$S" tap 590 1400 > /dev/null 2>&1; sleep 2
	"$S" swipe 590 1900 590 900 300 > /dev/null 2>&1; sleep 2
	"$S" swipe 590 900 590 1900 300 > /dev/null 2>&1; sleep 1
	"$S" dismiss 1 > /dev/null 2>&1
	"$S" home > /dev/null 2>&1; sleep 3
	$FC wait home 30 > /dev/null 2>&1 || { ok=0; "$S" home > /dev/null 2>&1; sleep 3; }
	[ $ok = 1 ] || { bad=$((bad + 1)); "$S" screenshot "$O/fail-$rounds-${app##*.}.png" 3 > /dev/null 2>&1; }
	echo "$(date +%T) round $rounds $app $([ $ok = 1 ] && echo ok || echo FAIL)" >> "$O/rounds.log"
	if [ $(( $(date +%s) - last )) -ge ${SOAK_INTERVAL:-60} ]; then sample; last=$(date +%s); fi
done
sample
"$S" screenshot "$O/end.png" 3 > /dev/null 2>&1
python3 -I - "$O/samples.csv" > "$O/summary.txt" <<'EOF'
import csv, sys
r = list(csv.DictReader(open(sys.argv[1])))
a, b = r[0], r[-1]
print("soak: %.0f min, %s rounds (%s failed), cores %s, crash exits %s" % (int(b["secs"]) / 60, b["rounds"], b["failed_rounds"], b["cores"], b["crash_exits"]))
for k in ("darlingserver_mb", "mtlvk_mb", "backboardd_mb", "springboard_mb", "all_darling_mb"):
    v = [int(x[k]) for x in r if x[k]]
    if v:
        print("  %-17s start %6d  end %6d  max %6d MiB" % (k[:-3], v[0], v[-1], max(v)))
EOF
cat "$O/summary.txt"; echo "soak: samples in $O/samples.csv"
