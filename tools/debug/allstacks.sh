#!/bin/sh
# Heuristic per-thread backtraces for a Darling core (no unwind info for Mach-O code under gdb):
# x86Sim: cores and scratch files go to $X86SIM_SCRATCH (/tmp is a small tmpfs)
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
mkdir -p $X86SIM_SCRATCH
# symbolize every word on each thread's stack that points into a mapped image.
#   tools/debug/allstacks.sh <pid of a coredumpctl core | core file> [words]

if [ -f "$1" ]; then cp "$1" $X86SIM_SCRATCH/allstacks.core; else coredumpctl dump "$1" -o $X86SIM_SCRATCH/allstacks.core >/dev/null 2>&1; fi
readelf -n $X86SIM_SCRATCH/allstacks.core 2>/dev/null | awk '/Page size:/{print "pagesize", $3} /NT_FILE/{f=1} f && /^ +0x/ {s=$1; e=$2; o=$3; getline p; gsub(/^ +/,"",p); print s, e, o, p}' > $X86SIM_SCRATCH/allstacks.maps
n=$(gdb -batch -ex 'info threads' $X86SIM_PREFIX/libexec/darling/usr/libexec/darling/mldr $X86SIM_SCRATCH/allstacks.core 2>/dev/null | grep -cE '^\*? +[0-9]+ ')
for t in $(seq 1 "$n"); do
	echo "== thread $t"
	gdb -batch -ex "thread $t" -ex "x/${2:-1500}gx \$rsp" $X86SIM_PREFIX/libexec/darling/usr/libexec/darling/mldr $X86SIM_SCRATCH/allstacks.core 2>/dev/null \
		| sed 's/^[^:]*://' | grep -oE '0x0000[0-9a-f]{12}' \
		| xargs python3 -I $X/tools/debug/symbolize_core.py $X86SIM_SCRATCH/allstacks.maps 2>/dev/null \
		| awk '$2 != "?" && $0 !~ /no segment/ {print "   ", $2, $3}' | awk '!s[$0]++' | head -${FRAMES:-30}
done
