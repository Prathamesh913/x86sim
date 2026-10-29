#!/bin/bash
# x86Code: FSEvents event history across boots (host/fseventsd journal + catch-up; tests/probes/fsevhist).
# Boots twice (needs SUDO_PASS for the shutdowns): boot 1 records an event id and creates a file; between the boots
# the tree is changed from Linux; boot 2 asks for the events since the recorded id. Leaves boot 2 running.
#   tests/fsevhist.sh
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT; S="$X/cli/x86sim"
D="$X86SIM_DEVICE/data/tmp/fsevhist"; L="$X86SIM_DEVICE/data/var/log/x86code-fsevhist.log"
JOB="com.x86code.fsevhist=tests/probes/fsevhist|/Volumes/SystemRoot$X86SIM_DEVICE/data/tmp/fsevhist"
waitfor() { for _ in $(seq 120); do grep -q "$1" "$L" 2>/dev/null && return 0; sleep 1; done; return 1; }
"$S" shutdown > /dev/null 2>&1
rm -rf "$D"; rm -f "$L"
"$S" boot "$JOB" > /dev/null || { echo "fsevhist: boot 1 failed"; exit 1; }
waitfor '^boot 1:' || { echo "fsevhist: no boot 1 result"; exit 1; }
"$S" shutdown > /dev/null 2>&1
# between the boots, from Linux
echo two > "$D/h/between-boots"
sleep 1.1; echo more >> "$D/h/during-boot1"
"$S" boot "$JOB" > /dev/null || { echo "fsevhist: boot 2 failed"; exit 1; }
waitfor '^no MustScanSubDirs' || { echo "fsevhist: no boot 2 result"; cat "$L"; exit 1; }
grep -E 'PASS|FAIL' "$L"
! grep -q FAIL "$L"
