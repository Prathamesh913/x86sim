#!/bin/sh
# Core a running Darling process (cmdline contains $1) and show its main thread and any
# thread blocked in IPC/locks/kqchan: tools/debug/stuck.sh <pattern> [frames] (needs SUDO_PASS)
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT

c=$($X/tools/debug/gcoresim.sh "$1") || exit 1
FRAMES=${2:-30} $X/tools/debug/allstacks.sh "$c" 6000 > $X86SIM_SCRATCH/stuck.txt 2>&1
sed -n '/thread 1$/,/thread 2$/p' $X86SIM_SCRATCH/stuck.txt | grep -vE "libkqueue|libsystem_kernel|libsystem_malloc|libsystem_pthread|\+0x0$" | cut -c1-130 | head -${MAIN:-16}
awk '/^== thread/{t=$0} /reply_sync|CFPREFERENCES_IS_WAITING|_xpc_pipe_routine|unfair_lock_lock_slow|evfilt_machport_copyout|semaphore_wait|send_and_wait/{print t": "$2}' $X86SIM_SCRATCH/stuck.txt | grep -v "thread 1:" | sort -u
