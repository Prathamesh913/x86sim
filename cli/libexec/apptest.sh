#!/bin/sh
# x86Code: app test pass: cli/libexec/apptest.sh <out dir> [bundle id ...]
# On a running boot (cli/libexec/rundev.sh; permission answers pre-seeded by cli/libexec/seedperms.py), opens each app with
# `simtouch.py app`, answers any permission prompt still on screen (`simtouch.py dismiss`, OCR), screenshots it
# (<out dir>/NN-<bundle id>.png) and goes home. Then collects the crashes since it started: launchd's "exited due"
# lines (exits.txt) and the cores (cores.txt, signal + process).
# APPWAIT: seconds an app gets to draw (default 20).
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
O=$1; shift
mkdir -p "$O"
APPS=${*:-"com.apple.mobilesafari com.apple.news com.apple.Preview com.apple.DocumentsApp com.apple.Preferences com.apple.Maps com.apple.Health com.apple.mobilecal com.apple.Fitness com.apple.Passbook com.apple.MobileSMS com.apple.MobileAddressBook com.apple.Passwords com.apple.reminders com.apple.mobileslideshow com.apple.shortcuts"}
start=$(date "+%Y-%m-%d %H:%M:%S")
shot() { python3 $X/cli/libexec/fb2png.py $X86SIM_DEVICE/data/framebuffer.bgra "$1" 3 > /dev/null; }
python3 $X/cli/libexec/simtouch.py dismiss   # prompts on the home screen (e.g. widget location)
shot "$O/00-home.png"
i=1
for b in $APPS; do
	python3 $X/cli/libexec/simtouch.py app $b
	sleep $((${APPWAIT:-20} / 2))
	python3 $X/cli/libexec/simtouch.py dismiss
	sleep $((${APPWAIT:-20} / 2))
	python3 $X/cli/libexec/simtouch.py dismiss
	shot "$O/$(printf %02d $i)-$b.png"
	python3 $X/cli/libexec/simtouch.py home; sleep 4
	i=$((i+1))
done
cp $X86SIM_STATE/rundev-spawns.log "$O/spawns.log"
awk -v s="$start" 'substr($0,1,19) >= s' "$O/spawns.log" | grep "exited due" | grep -v "exit(0)" | \
	sed 's/^[0-9-]* [0-9:.]* //; s/\[[0-9]*\]//g; s/pid\/[0-9]*/pid/; s/, ran for.*//; s/<Notice>: //' | sort | uniq -c | sort -rn > "$O/exits.txt"
coredumpctl list --no-legend --since "$start" 2>/dev/null | awk '{print $5, $8}' | while read pid sig; do
	echo "$sig $(coredumpctl info $pid 2>/dev/null | grep -m1 'Command Line' | sed 's/.*\///; s/'"'"'.*//; s/ .*//')"
done | sort | uniq -c | sort -rn > "$O/cores.txt"
cat "$O/exits.txt" "$O/cores.txt"
