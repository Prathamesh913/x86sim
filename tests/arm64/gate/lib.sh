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
# shot PNG: the ring's newest frame; its frame number goes to $O/.frame
shot() { timeout 12 python3 cli/libexec/fb2png.py "$SIMFB_RING_LINUX" "$1" 2>/dev/null | sed -n 's/^frame \([0-9]*\):.*/\1/p' > "$O/.frame"; }
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
# Speed 27: logs first. A check reads the screen only once the logs say the app is alive: its process is running and
# its per-process JIT log has no crash since mark_logs. (A screenshot of something that already crashed is wasted.)
# Speed 31: only this phone's processes (another phone on the host runs the same names): a guest process's
# environment names its device dir (TMPDIR=<view>$DEV/data/tmp/, X86SIM_DEVICE).
of_phone() { tr '\0' '\n' < "/proc/$1/environ" 2>/dev/null | grep -qF "$(realpath -m "$DEV")/data"; }
pids_of() { local p; for p in $(ps -eo pid=,comm= | awk -v n="$1" '$2==n{print $1}'); do of_phone "$p" && echo "$p"; done; }
pid_of() { pids_of "$1" | head -1; }
# crashed_proc NAME: a crash line in NAME's JIT logs since mark_logs
crashed_proc() {
	for f in "$LOGB"/"$1".*.log; do
		[ -f "$f" ] || continue
		local o; o=$(awk -v f="$f" '$1==f{print $2}' "$O/.sizes" 2>/dev/null)
		tail -c +$(( ${o:-0} + 1 )) "$f" 2>/dev/null | grep -Eq "$CRASHRE" && return 0
	done
	return 1
}
# proc_state NAME: alive | crashed | gone (gone: not running and no crash logged, e.g. it exited)
proc_state() {
	crashed_proc "$1" && { echo crashed; return; }
	[ -n "$(pid_of "$1")" ] && echo alive || echo gone
}
# wait_proc LABEL NAME MAXSECS: the app's process comes up (polled from the process list, no screen)
wait_proc() {
	local i
	for i in $(seq 1 "$3"); do
		case $(proc_state "$2") in
			alive) return 0 ;;
			crashed) say "CRASH $1: $2 crashed at launch ($(ls -t "$LOGB"/"$2".*.log 2>/dev/null | head -1 | xargs -r basename))"; return 5 ;;
		esac
		sleep 1
	done
	say "NOPROC $1: $2 not running after $3 s"; return 6
}
# summed CPU seconds (whole) of this phone's guest processes (runtime binaries); changes while the phone works
guest_cpu() { ps -eo times=,args= 2>/dev/null | awk '/RuntimeRoot|runtimes\/ios-/ {s += $1} END {print s + 0}'; }
# wait_ocr LABEL REGEX MAXSECS: poll every 2 s (GATE_SLOW=n: n x the limits, for JIT-only training boots); HIT/STALL (30 s without any screen or CPU change)/TIMEOUT
wait_ocr() {
	local t0 last="" flat=0 o el
	t0=$(date +%s.%N)
	while :; do
		sleep 2
		if [ -n "${PROC:-}" ]; then
			case $(proc_state "$PROC") in
				crashed) say "CRASH $1: $PROC crashed"; return 5 ;;
				gone) say "GONE $1: $PROC is no longer running"; return 6 ;;
			esac
		fi
		o=$(ocr); el=$(python3 -c "print(f'{$(date +%s.%N)-$t0:.0f}')")
		# FRESH=1: the status-bar clock must show the current (or last) minute, so a frame left in the ring from the
		# previous boot doesn't count
		local fresh=1
		if [ "${FRESH:-0}" = 1 ]; then
			fresh=0
			for m in 0 1; do echo "$o" | grep -q "$(date -d "-$m min" +%-I:%M)" && fresh=1; done
			# Speed 26: and not the previous boot's last frame (STALEFRAME: its ring frame number, gate.sh), which
			# shows the same minute when boots follow each other closely
			[ -n "${STALEFRAME:-}" ] && [ "$(cat "$O/.frame" 2>/dev/null)" = "$STALEFRAME" ] && fresh=0
		fi
		if [ $fresh = 1 ] && echo "$o" | grep -Eqi "$2"; then cp "$O/now.png" "$O/$1.png"; say "HIT $1 t+${el}s [${o:0:110}]"; return 0; fi
		# Speed 26: a static screen with the phone's processes still using CPU (the boot logo while lsd/datamigrator
		# work) is not a stall: their summed CPU seconds are part of the signature
		local sig; sig="$o $(md5sum < "$O/now.png") $(guest_cpu)"
		[ "$sig" = "$last" ] && flat=$((flat + 2)) || flat=0; last=$sig
		[ $flat -ge 30 ] && { cp "$O/now.png" "$O/$1-stall.png"; say "STALL $1 t+${el}s [${o:0:110}]"; return 3; }
		[ "${el%.*}" -ge $(( $3 * ${GATE_SLOW:-1} )) ] && { cp "$O/now.png" "$O/$1-timeout.png"; say "TIMEOUT $1 [${o:0:110}]"; return 4; }
	done
}
# open_check LABEL BUNDLE REGEX MAX [PROC]: from home, open; logs first (the process comes up, no crash), then wait for
# the regex on screen while the process stays alive. PROC: the app's process name (default: from the bundle id).
open_check() {
	local p=${5:-$(proc_for "$2")}
	home 6; app "$2"
	if [ -n "$p" ]; then wait_proc "$1" "$p" 60 || return $?; fi
	PROC=$p wait_ocr "$1" "$3" "$4"
}
proc_for() {
	case $1 in
		com.apple.Preferences) echo Preferences ;; com.apple.mobilecal) echo MobileCal ;;
		com.apple.mobileslideshow) echo Photos ;; com.apple.Maps) echo Maps ;;
		com.apple.DocumentsApp) echo Files ;; com.apple.Health) echo Health ;; com.apple.Passbook) echo Passbook ;;
		com.apple.MobileSMS) echo MobileSMS ;; com.apple.news) echo News ;; com.apple.mobilesafari) echo MobileSafari ;;
	esac
}
pss() { "$AOT" sim status 2>/dev/null | grep -o 'procs [0-9]*\|PSS [0-9]* MB' | paste -sd' '; }
# tap_word WORD: OCR the current frame (TSV) and tap the centre of the first word equal to WORD; 1 if not on screen
tap_word() {
	shot "$O/now.png"
	local xy
	xy=$(timeout 20 tesseract "$O/now.png" stdout -c tessedit_create_tsv=1 2>/dev/null |
		awk -F'\t' -v w="$1" 'NF==12 && $12==w {print int($7+$9/2), int($8+$10/2); exit}')
	[ -n "$xy" ] || return 1
	touch_ tap $xy
}
# photos_whatsnew LABEL: Photos shows "What's New in Photos" on an open after an update or a fresh install (as on a
# Mac's simulator): record it, dismiss it with Continue and wait for the library (Speed 26)
photos_whatsnew() {
	ocr | grep -qi "What.s New" || return 0
	say "$1: What's New in Photos sheet (expected on a fresh install): Continue"
	# OCR rarely reads the white-on-blue button: its place on the sheet (bottom centre, iPhone 17 / 1206x2622) instead
	tap_word Continue || touch_ tap 603 2430
	sleep 3; touch_ dismiss  # then Photos asks to send notifications
	wait_ocr "$1-after" 'Library|Collections|No Photos|Recent|Search' 60
}
# maps_tiles LABEL (Speed 27): Maps is up (logs first), then its map area must show tiles (maptiles.py: water and
# land colours); polled up to 90 s while Maps stays alive. Saves the frame as LABEL.png and logs the colour counts.
maps_tiles() {
	local i r st
	for i in $(seq 1 18); do
		st=$(proc_state Maps); [ "$st" = alive ] || { say "FAIL $1: Maps $st"; return 5; }
		shot "$O/now.png"; r=$(python3 "$X/tests/arm64/gate/maptiles.py" "$O/now.png")
		case $r in "tiles yes"*) cp "$O/now.png" "$O/$1.png"; say "HIT $1 [$r]"; return 0 ;; esac
		sleep 5
	done
	cp "$O/now.png" "$O/$1-fail.png"; say "FAIL $1: no map tiles drawn [$r]"; return 4
}
# app_switcher LABEL (Speed 27): Settings opened, then from home swipe up from the bottom edge and hold: SpringBoard's switcher
# shows the recent apps as cards, each with its name and icon above it (the front card's label in the band y 250-450 px
# on a 1206x2622 frame) and no status-bar clock; home again afterwards. (A frame of an app full screen or of the home
# screen has no app name in that band.)
app_switcher() {
	local i xy
	open_check "$1-app" com.apple.Preferences 'Airplane|Wi-Fi|Bluetooth|General|Model Name|iOS Version|About' 60 || return $?  # Speed 34: About kept (gate.sh)
	for i in 1 2 3 4 5; do
		# from the home screen (Settings is the most recent app), as the gesture is made on a phone at rest
		home 5; touch_ swipe 603 2615 603 1700 400 300; sleep 4
		shot "$O/now.png"
		xy=$(timeout 20 tesseract "$O/now.png" stdout -c tessedit_create_tsv=1 2>/dev/null |
			awk -F'\t' 'NF==12 && $12 ~ /^(Settings|Photos|Maps|Messages|News|Safari|Wallet|Health|Files|Calendar)$/ {y=$8+$10/2; if (y>=250 && y<=450) {print $12, int(y); exit}}')
		if [ -n "$xy" ]; then cp "$O/now.png" "$O/$1.png"; say "HIT $1 [card label: $xy]"; home 5; return 0; fi
		sleep 2
	done
	cp "$O/now.png" "$O/$1-fail.png"; say "FAIL $1: no app switcher cards"; home 5; return 4
}
# health_welcome LABEL (Speed 27): Health's first-run "Welcome to Health" (as on a Mac's simulator after a fresh install
# or reset of its data): recorded, continued through (Continue / Get Started / Not Now / Done on each page), then the
# Summary must show.
health_welcome() {
	ocr | grep -qi "Welcome to Health" || return 0
	say "$1: Welcome to Health (first run): continuing through it"
	local i
	for i in 1 2 3 4 5 6; do
		ocr | grep -Eq 'Summary|Pinned|Health Data|Sharing' && break
		tap_word Continue || tap_word Started || tap_word Now || tap_word Done || tap_word Next || touch_ tap 603 2430
		sleep 4; touch_ dismiss
	done
	PROC=Health wait_ocr "$1-after" 'Summary|Pinned|Health Data|Sharing' 60
}
