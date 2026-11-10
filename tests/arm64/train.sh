#!/bin/bash
# tests/arm64/train.sh [TAG] (Speed 25): a training pass for a phone's AOT image. Cold boot, the gate's app pass
# (Settings + General > About, Calendar, Photos, Maps, keyboard, Siri, Files, Health, Wallet), then Messages (compose +
# keyboard), News, Safari (a page), more Settings pages and the app switcher; the phone is stopped (every process
# saves its JIT profile into the cache dir) and `x86sim-aot --device D update` rebuilds the image from them.
# A64_DEVICE=<device> (default var/device-a64). NOUPDATE=1: no image rebuild. NOSTOP=1: leave the phone running
# (no update then). Logs in $GATE_OUT/<TAG> (default ~/scratch/x86sim/gate/train).
. "$(dirname "$0")/gate/lib.sh"
# the processes load the image; their JIT profiles (image blocks + newly translated ones) are training input
export X86SIM_A64_AOT_AUTOSAVE=1
T=${1:-train}; export GATE_OUT=${GATE_OUT:-$SCR/gate}; O=$GATE_OUT/$T
"$(dirname "$0")/gate/gate.sh" "$T"; echo "gate: $? failed checks"
mkdir -p "$O"
say "--- extra training $(date +%T)"
# Messages: compose, type into the To field and the body
home 6; app com.apple.MobileSMS; wait_ocr messages 'Messages|Edit|Filters|iMessage|Start' 120
touch_ tap 1100 260; sleep 6; touch_ text hello; sleep 3; shot "$O/messages-compose.png"
home 6; app com.apple.news; wait_ocr news 'News|Today|Following|Sports|Audio|Continue|Welcome' 120; sleep 15
shot "$O/news-after.png"
home 6; open_ 'https://www.apple.com'; wait_ocr safari 'Apple|Safari|apple.com|iPhone' 120; sleep 5
for p in 'prefs:root=General' 'prefs:root=DISPLAY' 'prefs:root=Sounds' 'prefs:root=Privacy' 'prefs:root=General&path=Keyboard' \
	'prefs:root=WIFI' 'prefs:root=Bluetooth' 'prefs:root=BATTERY_USAGE' 'prefs:root=General&path=STORAGE_MGMT'; do
	home 3; open_ "$p"; sleep 8
done
# Speed 27: Maps' place card and directions (SwiftUI route views; translated live they missed AttributeGraph's update
# deadline and Maps hit a fatal precondition), three times so their code gets into the profile
for i in 1 2 3; do "$(dirname "$0")/gate/mapsroute.sh" "mapsroute$i"; done
# app switcher (swipe up + hold), then home
home 4; touch_ swipe 603 2615 603 1700 400 300; sleep 4; shot "$O/switcher.png"; home 6
c=$(crashes); [ -n "$c" ] && say "CRASH in: $c"
say "--- extra training done $(date +%T)"
[ "${NOSTOP:-0}" = 1 ] && exit 0
A64_STOP_ONLY=1 A64_DEVICE="$DEV" cli/libexec/arm64/boot-a64.sh >/dev/null 2>&1
[ "${NOUPDATE:-0}" = 1 ] && exit 0
nice -n 10 "$AOT" --device "$DEV" update 2>&1 | tail -5 | tee -a "$O/gate.log"
