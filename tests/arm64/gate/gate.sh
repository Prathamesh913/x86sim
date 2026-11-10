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
	# RAM 2 follow-up: Maps restores its last camera (a zoomed-in 3D Eiffel Tower after a route run: maps-tiles failed);
	# fileproviderd-vnode round: and MapsSync (the Louvre saved as "Work" opened its editor, no route). RAM 3: that was
	# not all of it: the camera is in the app group's defaults (MapsLastActivityCamera), and SpringBoard's launch
	# snapshot of the last route view (SplashBoard + applicationState.db) survived too. reset_scene_state (lib.sh) drops
	# all of it for Maps, and Settings' restored page for Settings.
	reset_scene_state com.apple.Maps com.apple.Preferences
	# RAM 3: and an open request nobody consumed (a test that ran while no phone was up: r3k3's boot hung, mapsroute's
	# three "app com.apple.Maps" waited in the trigger file and the next boot opened Maps over the home check, r3l1)
	rm -f "$DEV"/data/tmp/x86sim-openurl "$DEV"/data/tmp/x86sim-openurl.new
	OLDSB=$(pids_of SpringBoard | tr '\n' ' ')
	A64_KEEP=1 X86SIM_METAL=all SECS=20 A64_DEVICE="$DEV" setsid cli/libexec/arm64/boot-a64.sh > "$O/boot.log" 2>&1 < /dev/null &
	for i in $(seq 1 60); do sleep 2; sb=$(pids_of SpringBoard | grep -vw -e "${OLDSB:-x}" | head -1); [ -n "$sb" ] && break; done
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
# Speed 31 (info): the first keys after the keyboard comes up arrive whole (simhid queues them while it attaches)
ocr | grep -qiw 'wifi' && say "first keys: wifi typed whole" || say "first keys: 'wifi' not read back whole (info)"
# Speed 27: and more letters (each one queries the parser and suggestions); Spotlight must not crash on any of them
touch_ text " photos"; sleep 8; crashed_proc Spotlight && { say "CRASH keyboard: Spotlight crashed while typing"; FAIL=$((FAIL + 1)); }
# Siri (Speed 26): hold the side button (iPhone 17 has no home button; on a home-button model the long press is the
# home button's) and count Siri's UI processes (assistantd alone runs anyway, so it is not counted)
home 5; touch_ raw "button lock down"; sleep 2.5; touch_ raw "button lock up"; sleep 4; shot "$O/siri.png"; sleep 6
siri=$(for n in Siri Campo SiriViewService; do pids_of $n; done | grep -c .)
# Speed 32: Siri's first launch after boot can miss the first long press (also with Speed 31's bits: 2 of 2 fresh boots
# missed the first press 150 s after boot and caught the second); one more press before it counts as a failure
if [ "$siri" = 0 ]; then
	say "siri: no UI process after the first press, pressing again"
	home 5; touch_ raw "button lock down"; sleep 2.5; touch_ raw "button lock up"; sleep 10
	siri=$(for n in Siri Campo SiriViewService; do pids_of $n; done | grep -c .)
fi
say "siri UI processes: $siri"; [ "$siri" -gt 0 ] || FAIL=$((FAIL + 1))
f open_check files com.apple.DocumentsApp 'On My iPhone|Browse|Recents|Shared' 120
f open_check health com.apple.Health 'Summary|Pinned|Health Data|Sharing|Welcome to Health' 120 && f health_welcome health
f open_check wallet com.apple.Passbook 'Wallet|Passes|Apple Pay|Tickets' 120
# Speed 27: Messages, News, Safari with a page
f open_check messages com.apple.MobileSMS 'Messages|Edit|Filters|New Message|Start a conversation' 120
f open_check news com.apple.news 'Today|News\+|Following|Audio|Sports|Puzzles|Top Stories' 150
home 5; open_ 'https://en.wikipedia.org/wiki/Paris'; f wait_proc safari MobileSafari 60 && PROC=MobileSafari f wait_ocr safari 'Paris|Wikipedia|France' 150
# Safari 1: google.com search (tap the box, type, return): WebContent faulted on the tap (an XPC shmem whose data starts
# mid-page mapped one page short, darlingserver mach_vm_map RETURN_DATA_ADDR)
google_search() {
	open_ 'https://www.google.com'; wait_ocr google-home 'Google|Trending|Sign in' 90 || return 1
	touch_ tap 400 805; sleep 4; timeout 30 python3 cli/libexec/simtouch.py text 'wikipedia linux' >/dev/null 2>&1; sleep 2
	touch_ key 40; PROC=MobileSafari wait_ocr google-results 'Wikipedia|Linux kernel|Torvalds' 90 || return 1
	! crashed_proc com.apple.WebKit.WebContent || { say "CRASH google-search: WebContent"; return 5; }
}
f google_search
# Speed 29: Maps route (search, Plan, origin, route). Speed 36: on by default (GATE_ROUTE=0 skips it): the AttributeGraph
# abort was darlingserver's shared vm_remap losing the old table's aliasing on the second growth.
[ "${GATE_ROUTE:-1}" = 1 ] && O="$O/route" f tests/arm64/gate/mapsroute.sh maps-route
# Speed 27: home by swipe-up from an app, then the app switcher (swipe up and hold) shows the recent apps
# Speed 34: Settings may still be alive from the About check (an aged system: jetsam's grace doubles per kill) and
# then correctly resumes on About (iOS keeps the page): accept that page too (the check is "an app is showing").
f open_check swipeup-app com.apple.Preferences 'Airplane|Wi-Fi|Bluetooth|General|Model Name|iOS Version|About' 60
touch_ swipe 603 2610 603 1500 250; f wait_ocr swipeup-home 'Calendar|Photos|Wallet|Health' 30
f app_switcher switcher
home 15
c=$(crashes); [ -n "$c" ] && { say "CRASH in: $c"; FAIL=$((FAIL + 1)); }
say "PSS idle after the pass: $(pss)"
say "jetsam kills: $(grep -c killed "$X/var/run-a64/jetsam.log" 2>/dev/null)"
say "=== $T done: $FAIL failed"
exit $FAIL
