#!/bin/bash
# Runs inside cli/libexec/limited.sh (see boot-a64.sh). First darling touch creates darlingserver in
# this scope; join_prefix_cgroup moves any escapees so MemoryMax counts real guest RSS.
# Env: DPREFIX, X86SIM_DARLING, A64_TZNAME, A64_TIMEOUT, A64_CMDFILE
set -e
join_prefix_cgroup() {
	local self_cg p cmd cgfile pid
	self_cg=$(awk -F: '/^0::/{print $NF; exit}' /proc/self/cgroup 2>/dev/null)
	[ -n "$self_cg" ] || return 0
	cgfile=/sys/fs/cgroup${self_cg}/cgroup.procs
	[ -w "$cgfile" ] || return 0
	# one grep per pass (a tr + grep per host process forked ~1,300 times every 3 s)
	for p in $(grep -lsa -- "darlingserver" /proc/[0-9]*/cmdline 2>/dev/null); do
		pid=${p#/proc/}; pid=${pid%/cmdline}
		cmd=$(tr '\0' ' ' < "$p" 2>/dev/null)
		case "$cmd" in
			*"darlingserver ${DPREFIX}"*) echo "$pid" > "$cgfile" 2>/dev/null || true ;;
		esac
	done
	for p in $(grep -lsaz X86CODE_A64 /proc/[0-9]*/environ 2>/dev/null); do
		pid=${p#/proc/}; echo "${pid%/environ}" > "$cgfile" 2>/dev/null || true
	done
}
if [ -n "$A64_TZNAME" ] && [ -f "/usr/share/zoneinfo/$A64_TZNAME" ]; then
	timeout 30 "$X86SIM_DARLING" shell ln -sf "/usr/share/zoneinfo/$A64_TZNAME" /private/etc/localtime < /dev/null > /dev/null 2>&1 || true
fi
join_prefix_cgroup
(
	for _ in $(seq 1 30); do
		sleep 3
		join_prefix_cgroup
	done
) &
exec timeout "$A64_TIMEOUT" "$X86SIM_DARLING" shell sh -c "$(cat "$A64_CMDFILE")" < /dev/null
