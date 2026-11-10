#!/bin/bash
# x86Sim arm64: keep memory down for "home + one app" by reclaiming on-demand helpers
# SpringBoard starts that are not needed for a static home screen (Spotlight search host,
# WidgetRenderer, pkd plug-in kit when no extensions are installed). A64_JOBS=slim also
# omits sleepd + usernotificationsd (~400 MB RSS each); std keeps them for cold-boot BKS.
#
# Matches /proc/<pid>/comm (15-char truncated). Never touches SpringBoard, backboardd,
# or launchd_sim. A64_SLIM=0 disables. Interval: A64_SLIM_SECS (default 5;
# they grow to 400–600 MB RSS in ~15 s under the JIT, so a short interval matters).
set -euo pipefail
INTERVAL=${A64_SLIM_SECS:-5}
# comm names (kernel TASK_COMM_LEN=16 including NUL → 15 visible chars)
TARGETS='Spotlight WidgetRenderer_ pkd'
PREFIX=${A64_DPREFIX:-${X86SIM_SCRATCH:-$HOME/scratch/x86sim}/a64prefix}
LOG=${A64_STATE:-${X86SIM_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}/var/run-a64}/slim-reaper.log
mkdir -p "$(dirname "$LOG")"
echo "$(date -Iseconds) slim-reaper start interval=${INTERVAL}s targets=$TARGETS" >>"$LOG"

is_a64_guest() {
	local p=$1
	[ -r "/proc/$p/environ" ] || return 1
	grep -qaz X86SIM_A64 "/proc/$p/environ" 2>/dev/null
}

while true; do
	sleep "$INTERVAL"
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ -r "$p/comm" ] || continue
		comm=$(cat "$p/comm" 2>/dev/null) || continue
		case "$comm" in
			Spotlight|WidgetRenderer_*|pkd) ;;
			*) continue ;;
		esac
		is_a64_guest "$pid" || continue
		rss=$(awk '/^VmRSS/{print $2}' "$p/status" 2>/dev/null || echo 0)
		echo "$(date -Iseconds) kill $comm pid=$pid rss_mb=$((rss / 1024))" >>"$LOG"
		kill -KILL "$pid" 2>/dev/null || true
	done
done
