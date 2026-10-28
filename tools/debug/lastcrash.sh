#!/bin/sh
# Symbolize the newest Darling (mldr) core dump: tools/debug/lastcrash.sh [pid]
# x86Code: cores and scratch files go to $X86SIM_SCRATCH (/tmp is a small tmpfs)
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
mkdir -p $X86SIM_SCRATCH

PID=${1:-$(coredumpctl list --no-legend 2>/dev/null | grep mldr | tail -1 | awk '{print $5}')}
coredumpctl dump "$PID" -o $X86SIM_SCRATCH/last.core >/dev/null 2>&1
readelf -n $X86SIM_SCRATCH/last.core 2>/dev/null | awk '/NT_FILE/{f=1} f && /^ +0x/ {s=$1; e=$2; o=$3; getline p; gsub(/^ +/,"",p); print s, e, o, p}' > $X86SIM_SCRATCH/last.maps
coredumpctl info "$PID" 2>/dev/null | grep -E 'Signal:|Command Line:'
coredumpctl info "$PID" 2>/dev/null | grep -oE '#[0-9]+ +0x[0-9a-f]+' | awk '{print $2}' | head -${FRAMES:-25} \
 | xargs python3 -I $X/tools/debug/symbolize_core.py $X86SIM_SCRATCH/last.maps | sed "s#$X/##" | cut -c1-220
