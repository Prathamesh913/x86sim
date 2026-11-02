#!/bin/bash
# tests/arm64/gate/gate.sh TAG: one zero-crash gate run on the default arm64 device (as the launcher boots it):
# cold boot to home, Settings cold + warm, General > About, Calendar / Photos / Maps cold + warm, keyboard (Spotlight),
# Siri (side button), Files, Health, Wallet; Photos' What's New sheet is dismissed; crash scan of every process log; PSS idle and after the apps. Results in $O/gate.log.
# Speed 27: + Messages, News, Safari (loads a page), Maps tiles drawn (maptiles.py), Spotlight typing without a Spotlight
# crash, home by swipe-up from an app, the app switcher. Logs first: every app check waits for its process and fails on a
# crash or exit from the process list and its JIT log before it looks at the screen (lib.sh open_check / PROC).
# SKIPBOOT=1: use the running sim. Exit status: number of failed checks.
. "$(dirname "$0")/lib.sh"
T=${1:?tag}; O=${GATE_OUT:-$SCR/gate}/$T; rm -rf "$O"; mkdir -p "$O"; FAIL=0
f() { "$@" || FAIL=$((FAIL + 1)); }
say "=== $T $(date +%T)"
if [ "${SKIPBOOT:-0}" != 1 ]; then
	shot "$O/before.png"; STALEFRAME=$(cat "$O/.frame" 2>/dev/null)
	A64_STOP_ONLY=1 A64_DEVICE="$DEV" cli/libexec/arm64/boot-a64.sh >/dev/null 2>&1
	OLDSB=$(ps -eo pid,comm | awk '$2=="SpringBoard"{print $1}' | tr '\n' ' ')
	A64_KEEP=1 X86CODE_METAL=all SECS=20 A64_DEVICE="$DEV" setsid cli/libexec/arm64/boot-a64.sh > "$O/boot.log" 2>&1 < /dev/null &
	for i in $(seq 1 60); do sleep 2; sb=$(ps -eo pid,comm | awk '$2=="SpringBoard"{print $1}' | grep -vw -e "${OLDSB:-x}" | head -1); [ -n "$sb" ] && break; done
fi
mark_logs
FRESH=1 STALEFRAME=${STALEFRAME:-} f wait_ocr home 'Calendar|Photos|Wallet|Health' 300
sleep 8
f open_check settings_cold com.apple.Preferences 'Airplane|Wi-Fi|Bluetooth|General' 180
f open_check settings_warm com.apple.Preferences 'Airplane|Wi-Fi|Bluetooth|General' 60
home 4; open_ 'prefs:root=General&path=About'; f wait_ocr about 'Model Name|iOS Version|Model Number' 60
for spec in "calendar:com.apple.mobilecal:Today|Inbox|Calendars|October|November|New in Calendar" \
	"photos:com.apple.mobileslideshow:Library|Collections|No Photos|Recent|Search" \
	"maps:com.apple.Maps:Search Maps|Apple Maps|Places|Guides|Allow"; do
	IFS=: read n b re <<< "$spec"
	# Photos (Speed 26): the "What's New in Photos" sheet is accepted, recorded and dismissed (photos_whatsnew)
	[ "$n" = photos ] && re="$re|What.s New"
	f open_check "$n-cold" "$b" "$re" 120; [ "$n" = photos ] && f photos_whatsnew "$n-cold"
	f open_check "$n-warm" "$b" "$re" 60; [ "$n" = photos ] && f photos_whatsnew "$n-warm"
	# Speed 27: Maps must draw its map (tiles), not only its card: logs first (Maps alive), then the frame's colours
	[ "$n" = maps ] && f maps_tiles "$n-tiles"
done
sleep 15; say "PSS after apps: $(pss)"
# keyboard: Spotlight from the home screen (swipe down), type, look for the text and the keyboard's keys
home 5; touch_ swipe 603 900 603 1600; sleep 4; touch_ text wifi; f wait_ocr keyboard 'wifi|Wi-Fi' 40
# Speed 27: and more letters (each one queries the parser and suggestions); Spotlight must not crash on any of them
touch_ text " photos"; sleep 8; crashed_proc Spotlight && { say "CRASH keyboard: Spotlight crashed while typing"; FAIL=$((FAIL + 1)); }
# Siri (Speed 26): hold the side button (iPhone 17 has no home button; on a home-button model the long press is the
# home button's) and count Siri's UI processes (assistantd alone runs anyway, so it is not counted)
home 5; touch_ raw "button lock down"; sleep 2.5; touch_ raw "button lock up"; sleep 4; shot "$O/siri.png"; sleep 6
siri=$(ps -eo comm | grep -c -E '^(Siri|Campo|SiriViewService)$'); say "siri UI processes: $siri"; [ "$siri" -gt 0 ] || FAIL=$((FAIL + 1))
f open_check files com.apple.DocumentsApp 'On My iPhone|Browse|Recents|Shared' 120
f open_check health com.apple.Health 'Summary|Pinned|Health Data|Sharing|Welcome to Health' 120 && f health_welcome health
f open_check wallet com.apple.Passbook 'Wallet|Passes|Apple Pay|Tickets' 120
# Speed 27: Messages, News, Safari with a page
f open_check messages com.apple.MobileSMS 'Messages|Edit|Filters|New Message|Start a conversation' 120
f open_check news com.apple.news 'Today|News\+|Following|Audio|Sports|Puzzles|Top Stories' 150
home 5; open_ 'https://en.wikipedia.org/wiki/Paris'; f wait_proc safari MobileSafari 60 && PROC=MobileSafari f wait_ocr safari 'Paris|Wikipedia|France' 150
# Speed 29: Maps route (search, Plan, origin, route): GATE_ROUTE=1. Off by default while the AttributeGraph abort
# ("attribute failed to set an initial value: DynamicLayoutComputer<_VStackLayout>") is open (speed29.md).
[ "${GATE_ROUTE:-0}" = 1 ] && O="$O/route" f tests/arm64/gate/mapsroute.sh maps-route
# Speed 27: home by swipe-up from an app, then the app switcher (swipe up and hold) shows the recent apps
f open_check swipeup-app com.apple.Preferences 'Airplane|Wi-Fi|Bluetooth|General' 60
touch_ swipe 603 2610 603 1500 250; f wait_ocr swipeup-home 'Calendar|Photos|Wallet|Health' 30
f app_switcher switcher
home 15
c=$(crashes); [ -n "$c" ] && { say "CRASH in: $c"; FAIL=$((FAIL + 1)); }
say "PSS idle after the pass: $(pss)"
say "jetsam kills: $(grep -c killed "$X/var/run-a64/jetsam.log" 2>/dev/null)"
say "=== $T done: $FAIL failed"
exit $FAIL
