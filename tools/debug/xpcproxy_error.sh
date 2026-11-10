#!/bin/sh
# Print why xpcproxy_sim fails to start a job (it exits 78 after formatting a message).
# Needs a running launchd_sim whose job respawns (mksimdevice.py with X86SIM_KEEPALIVE=1).
# strace makes its exit_group fail, so it faults and leaves a core with the message.
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT

s() { x86sim_sudo "$@"; }
L=$(pgrep -f "^/Volumes/SystemRoot$X/runtimes/.*launchd_sim" | head -1)
[ -n "$L" ] || { echo "no launchd_sim running"; exit 1; }
s timeout "${WAIT:-14}" strace -f -e trace=exit_group -e inject=exit_group:error=ENOSYS:when=1 -o $X86SIM_SCRATCH/xpcproxy_error.st -p "$L" 2>/dev/null
P=$(grep -m1 "exit_group(78).*INJECTED" $X86SIM_SCRATCH/xpcproxy_error.st | awk '{print $1}')
[ -n "$P" ] || { echo "no xpcproxy_sim exit(78) seen"; exit 1; }
sleep 3
coredumpctl dump "$P" -o $X86SIM_SCRATCH/xpcproxy_error.core >/dev/null 2>&1
strings -a $X86SIM_SCRATCH/xpcproxy_error.core | grep -E "Missing program|Unable to unpack|Invalid environment\[|No space for special|Invalid special port\[|Already registered|setup_fds bad|lwcr provided|Insufficient spawnattr|Special ports invalid|Unable to set current|X_OK\) failed|posix_spawn\(" | sort -u
