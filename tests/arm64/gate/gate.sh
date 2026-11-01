#!/bin/bash
# tests/arm64/gate/gate.sh TAG: one zero-crash gate run on the default arm64 device (as the launcher boots it):
# cold boot to home, Settings cold + warm, General > About, Calendar / Photos / Maps cold + warm, keyboard (Spotlight),
# Siri, Files, Health, Wallet; crash scan of every process log; PSS idle and after the apps. Results in $O/gate.log.
# SKIPBOOT=1: use the running sim. Exit status: number of failed checks.
. "$(dirname "$0")/lib.sh"
T=${1:?tag}; O=${GATE_OUT:-$SCR/gate}/$T; rm -rf "$O"; mkdir -p "$O"; FAIL=0
f() { "$@" || FAIL=$((FAIL + 1)); }
say "=== $T $(date +%T)"
if [ "${SKIPBOOT:-0}" != 1 ]; then
	A64_STOP_ONLY=1 A64_DEVICE="$DEV" cli/libexec/arm64/boot-a64.sh >/dev/null 2>&1
	OLDSB=$(ps -eo pid,comm | awk '$2=="SpringBoard"{print $1}' | tr '\n' ' ')
	A64_KEEP=1 X86CODE_METAL=all SECS=20 A64_DEVICE="$DEV" setsid cli/libexec/arm64/boot-a64.sh > "$O/boot.log" 2>&1 < /dev/null &
	for i in $(seq 1 60); do sleep 2; sb=$(ps -eo pid,comm | awk '$2=="SpringBoard"{print $1}' | grep -vw -e "${OLDSB:-x}" | head -1); [ -n "$sb" ] && break; done
fi
mark_logs
f wait_ocr home 'Calendar|Photos|Wallet|Health' 300
sleep 8
f open_check settings_cold com.apple.Preferences 'Airplane|Wi-Fi|Bluetooth|General' 180
f open_check settings_warm com.apple.Preferences 'Airplane|Wi-Fi|Bluetooth|General' 60
home 4; open_ 'prefs:root=General&path=About'; f wait_ocr about 'Model Name|iOS Version|Model Number' 60
for spec in "calendar:com.apple.mobilecal:Today|Inbox|Calendars|October|November|New in Calendar" \
	"photos:com.apple.mobileslideshow:Library|Collections|No Photos|Recent|Search" \
	"maps:com.apple.Maps:Search Maps|Apple Maps|Places|Guides|Allow"; do
	IFS=: read n b re <<< "$spec"
	f open_check "$n-cold" "$b" "$re" 120; f open_check "$n-warm" "$b" "$re" 60
done
sleep 15; say "PSS after apps: $(pss)"
# keyboard: Spotlight from the home screen (swipe down), type, look for the text and the keyboard's keys
home 5; touch_ swipe 603 900 603 1600; sleep 4; touch_ text wifi; f wait_ocr keyboard 'wifi|Wi-Fi' 40
# Siri: hold the home button
home 5; touch_ raw "button home down"; sleep 2; touch_ raw "button home up"; sleep 10; shot "$O/siri.png"
siri=$(ps -eo comm | grep -c -E '^(Campo|SiriViewService|assistantd)$'); say "siri processes: $siri"; [ "$siri" -gt 0 ] || FAIL=$((FAIL + 1))
f open_check files com.apple.DocumentsApp 'On My iPhone|Browse|Recents|Shared' 120
f open_check health com.apple.Health 'Summary|Pinned|Health Data|Sharing' 120
f open_check wallet com.apple.Passbook 'Wallet|Passes|Apple Pay|Tickets' 120
home 15
c=$(crashes); [ -n "$c" ] && { say "CRASH in: $c"; FAIL=$((FAIL + 1)); }
say "PSS idle after the pass: $(pss)"
say "jetsam kills: $(grep -c killed "$X/var/run-a64/jetsam.log" 2>/dev/null)"
say "=== $T done: $FAIL failed"
exit $FAIL
