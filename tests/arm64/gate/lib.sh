#!/bin/bash
# tests/arm64/gate/lib.sh (Speed 24): helpers for the arm64 zero-crash gate. Sourced; reads the screen with
# fb2png + tesseract (OCR), drives it with simtouch / the openurl trigger file. Output dir: $O.
# Speed 24 note: the earlier gate scripts lived only in scratch and were lost with it; these are kept in the repo.
X=${X86SIM_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}
SCR=${X86SIM_SCRATCH:-$HOME/scratch/x86code}
DEV=${A64_DEVICE:-$X/var/device-a64}
export SIMFB_RING_LINUX=${SIMFB_RING_LINUX:-/dev/shm/x86code-a64-simfb.ring}
export SIMHID_SOCKET=${SIMHID_SOCKET:-$DEV/data/simhid.sock} X86SIM_DEVICE=$DEV
export TESSDATA_PREFIX=${TESSDATA_PREFIX:-$SCR/tessdata}
LOGB=${A64_DPREFIX:-$SCR/a64prefix}/private/var/tmp/x86code-a64
AOT=$X/tools/launcher/target/release/x86code-aot
cd "$X"
say() { echo "$*" | tee -a "$O/gate.log"; }
shot() { timeout 12 python3 cli/libexec/fb2png.py "$SIMFB_RING_LINUX" "$1" >/dev/null 2>&1; }
ocr() { shot "$O/now.png"; timeout 20 tesseract "$O/now.png" stdout --psm 11 2>/dev/null | tr '\n' ' ' | tr -s ' '; }
touch_() { timeout 8 python3 cli/libexec/simtouch.py "$@" >/dev/null 2>&1; }
home() { touch_ home; sleep "${1:-5}"; }
open_() { printf '%s\n' "$1" > "$DEV/data/tmp/x86code-openurl.new"; mv "$DEV/data/tmp/x86code-openurl.new" "$DEV/data/tmp/x86code-openurl"; }
app() { open_ "app $1"; }
# crash lines in the per-process JIT logs since mark_logs
CRASHRE='signal 11|signal 4 |SIGSEGV|SIGILL|SIGABRT|guest exception|die:|fatal native'
mark_logs() { for f in "$LOGB"/*.log; do echo "$f $(stat -c %s "$f")"; done > "$O/.sizes" 2>/dev/null; }
crashes() {
	for f in "$LOGB"/*.log; do
		local o; o=$(awk -v f="$f" '$1==f{print $2}' "$O/.sizes" 2>/dev/null)
		tail -c +$(( ${o:-0} + 1 )) "$f" 2>/dev/null | grep -Eq "$CRASHRE" && echo "$(basename "$f")"
	done
}
# wait_ocr LABEL REGEX MAXSECS: poll every 2 s (GATE_SLOW=n: n x the limits, for JIT-only training boots); HIT/STALL (30 s without any screen or CPU change)/TIMEOUT
wait_ocr() {
	local t0 last="" flat=0 o el
	t0=$(date +%s.%N)
	while :; do
		sleep 2
		o=$(ocr); el=$(python3 -c "print(f'{$(date +%s.%N)-$t0:.0f}')")
		# FRESH=1: the status-bar clock must show the current (or last) minute, so a frame left in the ring from the
		# previous boot doesn't count
		local fresh=1
		if [ "${FRESH:-0}" = 1 ]; then
			fresh=0
			for m in 0 1; do echo "$o" | grep -q "$(date -d "-$m min" +%-I:%M)" && fresh=1; done
		fi
		if [ $fresh = 1 ] && echo "$o" | grep -Eqi "$2"; then cp "$O/now.png" "$O/$1.png"; say "HIT $1 t+${el}s [${o:0:110}]"; return 0; fi
		local sig; sig="$o $(md5sum < "$O/now.png")"
		[ "$sig" = "$last" ] && flat=$((flat + 2)) || flat=0; last=$sig
		[ $flat -ge 30 ] && { cp "$O/now.png" "$O/$1-stall.png"; say "STALL $1 t+${el}s [${o:0:110}]"; return 3; }
		[ "${el%.*}" -ge $(( $3 * ${GATE_SLOW:-1} )) ] && { cp "$O/now.png" "$O/$1-timeout.png"; say "TIMEOUT $1 [${o:0:110}]"; return 4; }
	done
}
# open_check LABEL BUNDLE REGEX MAX: from home, open, wait for the regex
open_check() { home 6; app "$2"; wait_ocr "$1" "$3" "$4"; }
pss() { "$AOT" sim status 2>/dev/null | grep -o 'procs [0-9]*\|PSS [0-9]* MB' | paste -sd' '; }
