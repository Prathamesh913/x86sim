#!/bin/bash
# x86Code: run a command (the simulator stack) in a systemd user scope with a disk read/write cap and low IO/CPU
# weight. A cold boot re-reads the runtime from disk whenever the page cache was evicted (~185 MB/s uncapped), which
# stalls the rest of the desktop; every process the stack spawns (apps, mtlvk, darlingserver) stays in the scope.
# X86SIM_IO_MAX bytes/s (default 40M; 0 = no scope), X86SIM_IO_IOPS (default 4000), X86SIM_IO_DEV (default: the
# block device holding the repo), X86SIM_CPU_WEIGHT (default 40; desktop apps are 100).
if [ "${X86SIM_IO_MAX:-40M}" = 0 ] || ! command -v systemd-run >/dev/null; then exec "$@"; fi
dev=${X86SIM_IO_DEV:-$(df --output=source "$(dirname "$0")" | tail -1)}
io=${X86SIM_IO_MAX:-40M}
exec systemd-run --user --scope --quiet --collect --unit="x86sim-$$" \
	-p IOWeight=10 -p "CPUWeight=${X86SIM_CPU_WEIGHT:-40}" \
	-p "IOReadBandwidthMax=$dev $io" -p "IOWriteBandwidthMax=$dev $io" \
	-p "IOReadIOPSMax=$dev ${X86SIM_IO_IOPS:-4000}" "$@"
