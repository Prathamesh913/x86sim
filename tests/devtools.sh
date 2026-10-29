#!/bin/bash
# x86Code developer-workflow test (simctl parity): with the DevTools test app (tests/apps/DevTools) checks
# x86sim listapps, get_app_container, launch --console with arguments and SIMCTL_CHILD_ environment, terminate,
# push, log stream --app, addmedia, io screenshot / recordVideo, pidof and debug (LLDB breakpoint + backtrace).
# x86sim location set / start (a route between waypoints) / list / clear, delivered to CLLocationManager in the app.
#   tests/devtools.sh [out dir]     (default $X86SIM_SCRATCH/devtools-<time>); needs a running boot (x86sim boot)
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT; S="$X/cli/x86sim"; B="$X/tests/apps/build"; ID=com.x86code.devtools
O=${1:-$X86SIM_SCRATCH/devtools-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$O"
fail=0
step() { printf '%-44s %s\n' "$1" "$2"; [ "$2" = PASS ] || fail=1; }
ok() { if "$@"; then echo PASS; else echo FAIL; fi; }
ps -eo args= | grep -q '[l]aunchd_sim' || { echo "devtools: no boot running (x86sim boot)"; exit 1; }
[ -d "$B/DevTools.app" ] || { echo "devtools: build the apps first: build/mac-build-apps.sh"; exit 1; }

"$S" listapps > "$O/listapps.txt" 2>&1
grep -q "^$ID " "$O/listapps.txt" || "$S" install "$B/DevTools.app" > "$O/install.txt" 2>&1
"$S" listapps > "$O/listapps.txt" 2>&1
step "listapps (DevTools, Settings)" "$(grep -q "^$ID .*User" "$O/listapps.txt" && grep -q '^com.apple.Preferences ' "$O/listapps.txt" && echo PASS || echo FAIL)"
app=$("$S" get_app_container $ID app 2>/dev/null); data=$("$S" get_app_container $ID data 2>/dev/null)
step "get_app_container app/data" "$([ -f "$app/Info.plist" ] && [ -d "$data/Library" ] && echo PASS || echo FAIL)"

# launch --console: arguments, SIMCTL_CHILD_ environment, stdout and stderr streamed here
SIMCTL_CHILD_X86_HELLO=world timeout 40 "$S" launch --console --console-secs 8 --terminate-running-process $ID -one two \
	> "$O/console.out" 2> "$O/console.err"
step "launch: arguments" "$(ok grep -q 'devtools: args -one two' "$O/console.out")"
step "launch: SIMCTL_CHILD_ environment" "$(ok grep -q 'X86_HELLO=world' "$O/console.out")"
step "launch --console: stdout" "$(ok grep -q 'devtools: tick 3' "$O/console.out")"
step "launch --console: stderr" "$(ok grep -q 'devtools: stderr tick 3' "$O/console.err")"
timeout 40 "$S" launch --console-pty --console-secs 6 --terminate-running-process $ID > "$O/console-pty.out" 2>&1
step "launch --console-pty: a tty, output here" "$(grep -q 'devtools: stdout tty true' "$O/console-pty.out" && grep -q 'devtools: stderr tick 2' "$O/console-pty.out" && echo PASS || echo FAIL)"
pid=$("$S" pidof $ID 2>/dev/null)
step "pidof" "$([ -n "$pid" ] && echo PASS || echo FAIL)"

# push: the app shows it in the foreground (willPresent) -> console line
timeout 40 "$S" launch --console --console-secs 12 --terminate-running-process $ID > "$O/push.out" 2>&1 &
sleep 4
printf '{"Simulator Target Bundle":"%s","aps":{"alert":{"title":"x86sim test","body":"push %s"}}}\n' $ID $$ > "$O/push.json"
"$S" push "$O/push.json" > "$O/push.txt" 2>&1
wait
step "push (delivered to the app)" "$(ok grep -q "notification x86sim test / push $$" "$O/push.out")"
"$S" screenshot "$O/push.png" 3 > /dev/null

# location: set (one point, repeated every second), start (route between two waypoints ~550 m apart at 100 m/s,
# an update per second: positions strictly between them), list (the runtime's scenarios), clear
timeout 60 "$S" launch --console --console-secs 30 --terminate-running-process $ID > "$O/location.out" 2>&1 &
sleep 4
"$S" location set 37.3349,-122.0090 > "$O/location.txt" 2>&1
sleep 5
"$S" location start --speed=100 --interval=1 37.3300,-122.0300 37.3350,-122.0300 >> "$O/location.txt" 2>&1
sleep 9
"$S" location list > "$O/location-list.txt" 2>&1
"$S" location clear >> "$O/location.txt" 2>&1
wait
step "location set (delivered to the app)" "$(ok grep -q 'devtools: location 37.3349,-122.0090' "$O/location.out")"
n=$(grep -oE 'devtools: location 37\.33[0-9]+,-122\.0300' "$O/location.out" | sort -u | awk -F'[ ,]' '$3 > 37.3300 && $3 < 37.3350' | wc -l)
step "location start (route: $n points between)" "$([ "$n" -ge 3 ] && echo PASS || echo FAIL)"
step "location list (scenarios)" "$(ok grep -q '^City Run$' "$O/location-list.txt")"

# terminate
"$S" terminate $ID > "$O/terminate.txt" 2>&1; step "terminate" "$([ $? = 0 ] && echo PASS || echo FAIL)"
sleep 1
step "terminate: not running afterwards" "$([ -z "$("$S" pidof $ID 2>/dev/null)" ] && echo PASS || echo FAIL)"

# log stream --app: the app's os_log lines
timeout 15 "$S" log stream --app $ID --match devtools-oslog > "$O/oslog.txt" 2>&1
step "log stream --app (os_log)" "$(ok grep -q 'devtools-oslog: tick' "$O/oslog.txt")"

# io screenshot / recordVideo
"$S" io screenshot "$O/io.png" > /dev/null 2>&1
step "io screenshot" "$(ok python3 -I -c 'import sys; from PIL import Image; sys.exit(0 if Image.open(sys.argv[1]).size == (1179, 2556) else 1)' "$O/io.png")"
"$S" io recordVideo --secs 2 "$O/rec.mp4" > /dev/null 2>&1
n=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$O/rec.mp4" 2>/dev/null)
step "io recordVideo (MP4, >= 60 frames)" "$([ "${n:-0}" -ge 60 ] && echo PASS || echo FAIL)"

# addmedia: count of assets in the Photos library database grows
db="$X86SIM_DEVICE/data/Media/PhotoData/Photos.sqlite"
before=$(sqlite3 "file:$db?mode=ro" 'select count(*) from ZASSET' 2>/dev/null || echo 0)
python3 -I -c 'import sys; from PIL import Image; Image.new("RGB", (640, 480), (40, 120, 200)).save(sys.argv[1])' "$O/media.jpg"
"$S" addmedia "$O/media.jpg" > "$O/addmedia.txt" 2>&1
sleep 2
after=$(sqlite3 "file:$db?mode=ro" 'select count(*) from ZASSET' 2>/dev/null || echo 0)
step "addmedia (Photos library $before -> $after)" "$([ "$after" -gt "$before" ] && echo PASS || echo FAIL)"

# debug: LLDB through debugserver in the container, breakpoint in the app, symbolic backtrace
if command -v lldb > /dev/null && [ -x "$X/host/debugserver/debugserver" ]; then
	timeout 240 "$S" debug --launch --port 12399 $ID -- -b -o 'breakpoint set -n "$s8DevTools14ViewControllerC11viewDidLoadyyF"' \
		-o continue -o 'bt 4' -o 'expr -l objc -O -- (id)[UIApplication sharedApplication]' \
		-o 'expr -l objc -- int $s = 0; for (int i = 0; i < 10; i++) $s += i; $s' -o kill > "$O/debug.txt" 2>&1
	step "debug: breakpoint hit" "$(ok grep -q 'stop reason = breakpoint 1' "$O/debug.txt")"
	step "debug: backtrace into UIKitCore" "$(ok grep -q 'UIKitCore`-\[UIViewController' "$O/debug.txt")"
	step "debug: po (ObjC call in the app)" "$(ok grep -q '^<UIApplication: 0x' "$O/debug.txt")"
	step "debug: expression with a loop (JIT)" "$(ok grep -q '(int) $[0-9]* = 45' "$O/debug.txt")"
else
	echo "debug                                        skipped (no lldb or host/debugserver/debugserver)"
fi
"$S" home > /dev/null
echo "devtools: $( [ $fail = 0 ] && echo all passed || echo FAILURES ) (logs in $O)"
exit $fail
