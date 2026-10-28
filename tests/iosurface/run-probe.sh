#!/bin/bash
# x86Code: boot a minimal simulator device with iosurfaced (com.apple.IOSurface.Remote) and the two-process
# IOSurface probe (tests/iosurface/iosprobe: "serve" = process A, plain = process B), print the logs, stop.
#   tests/iosurface/run-probe.sh   (SECS=40; X86CODE_IOSURFACE_LOG=1 for a server trace)
# Never run while another boot of var/device is running (tools/debug/killsim.sh first).
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
cd $X
D=$X86SIM_DEVICE
P=tests/iosurface/iosprobe
rm -f $D/data/var/log/com.x86code.iosprobe*.log $D/data/var/log/com.x86code.iosurfaced.log
find $D/data -name "*.lock" -type d -exec rm -rf {} + 2>/dev/null
X86CODE_IOSURFACE=1 X86CODE_NODT=all X86CODE_JOBLOG=1 python3 cli/libexec/mksimdevice.py $D \
	"com.x86code.iosprobe-a=$P|serve|mach:com.x86code.iosprobe" "com.x86code.iosprobe-b=$P" "$@" > /dev/null
TIMEOUT=$((${SECS:-40}+5)) cli/libexec/run-launchd-sim.sh $D > $X86SIM_STATE/lsim-ios.out 2>&1 &
for i in $(seq ${SECS:-40}); do
	sleep 1
	grep -q '^SUMMARY' $D/data/var/log/com.x86code.iosprobe-b.log 2>/dev/null && break
done
echo "=== process B"; cat $D/data/var/log/com.x86code.iosprobe-b.log 2>/dev/null
echo "=== process A"; cat $D/data/var/log/com.x86code.iosprobe-a.log 2>/dev/null
echo "=== iosurfaced"; tail -${LINES_HOST:-40} $D/data/var/log/com.x86code.iosurfaced.log 2>/dev/null
tools/debug/killsim.sh
