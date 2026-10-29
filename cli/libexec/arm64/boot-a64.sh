#!/bin/bash
# x86Code: boot the arm64 device (iOS 27.0 runtime, every process under libarm64jit; ARM64-PLAN stage 2) for SECS
# seconds and summarise. Everything is separate from the x86_64 iOS 26.5 stack, which keeps running untouched:
# its own Darling prefix (A64_DPREFIX, default $X86SIM_SCRATCH/a64prefix: only that container is shut down and
# restarted), device (var/device-a64), state dir (var/run-a64) and frame ring (/dev/shm/x86code-a64-simfb.ring).
# No mtlvk/mdns/jetsam restarts (those belong to the x86 boot). Runs in the systemd scope of cli/libexec/limited.sh
# (40 MB/s IO, memory cap X86SIM_MEM_MAX, default 6G).
# Usage: SECS=120 cli/libexec/arm64/boot-a64.sh [extra jobs...]   (A64_JOBS=min|std: the job list)
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}
export X86SIM_ARCH=arm64
export X86SIM_RUNTIME=${A64_RUNTIME:-$X86SIM_ROOT/runtimes/ios-27.0-24A434-arm64/RuntimeRoot}
export X86SIM_DEVICE=${A64_DEVICE:-$X86SIM_ROOT/var/device-a64}
export X86SIM_STATE=${A64_STATE:-$X86SIM_ROOT/var/run-a64}
. "$X86SIM_ROOT/cli/lib/config.sh"
export DPREFIX=${A64_DPREFIX:-$X86SIM_SCRATCH/a64prefix}
export X86SIM_DPREFIX=$DPREFIX
export X86CODE_METAL=${X86CODE_METAL-0}         # software rendering until metalhostd is arm64
export X86SIM_MEM_MAX=${X86SIM_MEM_MAX-6G}
X=$X86SIM_ROOT; cd "$X"
D=$X86SIM_DEVICE; ST=$X86SIM_STATE; SECS=${SECS:-90}
mkdir -p "$D" "$ST"
rm -rf "$D"/data/var/log/*.log
MIN="com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.notifyd com.apple.configd_sim
	com.apple.runningboardd com.apple.lsd com.apple.containermanagerd com.apple.containermanagerd.system
	com.apple.mobilegestalt.xpc com.apple.UserEventAgent-System com.apple.securityd com.apple.trustd com.apple.syslogd
	com.apple.mobile.installd com.apple.systemstatusd com.apple.distnoted.xpc.daemon com.apple.tccd
	com.apple.fontservicesd com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd"
# x86Code: lean UI set for the 6 GB budget (every arm64 process costs 150-450 MB under the JIT): no chronod (widgets;
# it spawns a widget extension process each) and no FileProvider. A64_JOBS=full adds them.
UI="com.apple.backboardd com.apple.SpringBoard com.apple.PosterBoard com.apple.sleepd com.apple.TextInput.kbd com.apple.FileCoordination
	com.apple.usernotificationsd"
FULLUI="com.apple.chronod com.apple.FileProvider"
case "${A64_JOBS:-std}" in
	min) JOBS="$MIN" ;;
	full) JOBS="$MIN $UI $FULLUI" ;;
	*) JOBS="$MIN $UI" ;;
esac
X86CODE_JOBLOG=1 python3 cli/libexec/mksimdevice.py "$D" $JOBS "$@" > /dev/null || exit 1
UDID=$(sed -n 's/^SIMULATOR_UDID=//p' "$D/sim.env")
L=$DPREFIX/private/var/tmp/com.apple.CoreSimulator.SimDevice.$UDID/launchd.log
rm -f "$L" "$L".1 "$L".2
R="/Volumes/SystemRoot$X86SIM_RUNTIME"
ENVS=$(sed 's/^/"/; s/$/"/' "$D/sim.env" | tr '\n' ' ')
ENVS="$ENVS ${A64_ENV:-}"   # A64_ENV: extra K=V for launchd_sim (e.g. X86CODE_A64_TRACE=1)
# stop only this prefix's container: its setuid launchers (timeout can't signal them) are found by their command
# line (this device's path, or tests/arm64/run.sh's a64home), so the x86_64 container's are never touched; then
# darling shutdown and stale state files
stop_prefix() {
	local pids="" d p
	for d in /proc/[0-9]*; do
		p=${d#/proc/}
		[ "$(tr '\0' '\n' < "$d/cmdline" 2>/dev/null | head -1)" = "$X86SIM_DARLING" ] || continue
		tr '\0' '\n' < "$d/cmdline" 2>/dev/null | grep -q -e "$D/" -e "/a64home" && pids="$pids $p"
	done
	[ -n "$pids" ] && x86sim_sudo kill -9 $pids 2>/dev/null
	timeout 30 "$X86SIM_DARLING" shutdown 2>/dev/null
	local ip; ip=$(cat "$DPREFIX/.init.pid" 2>/dev/null)
	if [ -z "$ip" ] || [ ! -d "/proc/$ip" ] || grep -q "^State:.*Z" "/proc/$ip/status" 2>/dev/null; then
		rm -f "$DPREFIX/.init.pid" "$DPREFIX/.darlingserver.sock"
	fi
}
stop_prefix
sleep 1
H="/Volumes/SystemRoot$X/host/simhost/simhost"
(cli/libexec/limited.sh timeout $([ "${A64_KEEP:-0}" = 1 ] && echo 86400 || echo $((SECS + 5))) "$X86SIM_DARLING" shell sh -c \
	"exec \"$H\" /usr/bin/env $ENVS \"$R/sbin/launchd_sim\" \"/Volumes/SystemRoot$D/data/var/run/launchd_bootstrap.plist\"" \
	< /dev/null > "$ST/lsim.out" 2>&1 &)
sleep 3
(timeout $((SECS + 2)) tail -n +1 -F "$L" 2>/dev/null | grep --line-buffered -v -E "failed lookup|Last log repeated" > "$ST/launchd.log" &)
sleep "$SECS"
echo "== spawns/exits"
grep -E "Successfully spawned|exited due|exited with" "$ST/launchd.log" | sed "s/^[0-9-]* [0-9:.]* //; s/\[[0-9]*\]//g; s/ because.*//; s/, ran for.*//; s/<Notice>: //" | sort | uniq -c | cut -c1-140
echo "== lsim.out (tail)"; tail -5 "$ST/lsim.out"
# A64_KEEP=1: leave the device running (the container outlives the launcher's timeout); else stop this prefix
[ "${A64_KEEP:-0}" = 1 ] || stop_prefix
