#!/bin/bash
# x86Code: boot a minimal simulator device with the Metal stand-in and run the test client.
#   tests/metal/run-probe.sh            (SECS=40 by default; MTLVK_LOG=1 / X86CODE_VKMTL_LOG=1 for traces)
# Starts mtlvk (Linux, Vulkan) on var/run/mtlvk.sock, creates var/device with only
# metalhostd (X86CODE_METAL=1: service name com.apple.metal.simulator.mtlprobe) and the mtlprobe job, boots
# launchd_sim for SECS seconds and prints the probe's and the service's logs. Never run while another
# boot of var/device is running (tools/debug/killsim.sh first).
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
cd $X
D=$X86SIM_DEVICE
MTLVK=${MTLVK:-$X86SIM_MTLVK}
SOCK=$X86SIM_STATE/mtlvk.sock; export X86CODE_MTLVK_SOCKET=$SOCK
rm -f $D/data/var/log/com.x86code.mtlprobe.log $D/data/var/log/com.x86code.metalhostd.log
find $D/data -name "*.lock" -type d -exec rm -rf {} + 2>/dev/null
"$MTLVK" "$SOCK" > $X86SIM_STATE/mtlvk.log 2>&1 &
MPID=$!
X86CODE_METAL=1 X86CODE_NODT=all X86CODE_JOBLOG=1 python3 cli/libexec/mksimdevice.py $D com.x86code.mtlprobe=tests/metal/mtlprobe "$@" > /dev/null
TIMEOUT=$((${SECS:-40}+5)) cli/libexec/run-launchd-sim.sh $D > $X86SIM_STATE/lsim-metal.out 2>&1 &
for i in $(seq ${SECS:-40}); do
	sleep 1
	grep -q '^SUMMARY' $D/data/var/log/com.x86code.mtlprobe.log 2>/dev/null && break
done
echo "=== mtlprobe"; cat $D/data/var/log/com.x86code.mtlprobe.log 2>/dev/null
echo "=== metalhostd"; grep -v '^\s*$' $D/data/var/log/com.x86code.metalhostd.log 2>/dev/null | tail -${LINES_HOST:-40}
echo "=== mtlvk"; grep -v 'metal2vulkan-' $X86SIM_STATE/mtlvk.log | tail -${LINES_VK:-20}
tools/debug/killsim.sh
kill $MPID 2>/dev/null
wait $MPID 2>/dev/null
