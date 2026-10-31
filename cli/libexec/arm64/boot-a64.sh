#!/bin/bash
# x86Code: boot the arm64 device (iOS 27.0 runtime, every process under libarm64jit; ARM64-PLAN stage 2) for SECS
# seconds and summarise. Everything is separate from the x86_64 iOS 26.5 stack, which keeps running untouched:
# its own Darling prefix (A64_DPREFIX, default $X86SIM_SCRATCH/a64prefix: only that container is shut down and
# restarted), device (var/device-a64), state dir (var/run-a64) and frame ring (/dev/shm/x86code-a64-simfb.ring).
# mdns/jetsam stay with the x86 boot. mtlvk is started here only when X86CODE_METAL!=0, on this boot's own
# socket ($ST/mtlvk.sock) and pidfile — never pkill -x mtlvk (would hit a frozen x86 stack's mtlvk).
# Runs in the systemd scope of cli/libexec/limited.sh (40 MB/s IO, memory cap X86SIM_MEM_MAX).
#
# Memory: Stage 2's 6G default was based on cgroup memory.current that missed every guest (darlingserver was
# started by a darling shell *outside* the scope — the TZ link — so guests inherited that foreign cgroup). Real
# Shared boot sum VmRSS at home is ~8.4–9.4 GB (PSS/cgroup ~4.6 GB). Default 12G so MemoryMax does not
# OOM on the sum-RSS guests (6–8G is too low for that accounting). Override with X86SIM_MEM_MAX.
# Usage: SECS=120 cli/libexec/arm64/boot-a64.sh [extra jobs...]   (A64_JOBS=min|std: the job list)
#        X86CODE_METAL=0|1|backboardd|all  (default 0 = software; needs host/metalhostd/arm64/metalhostd)
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}
export X86SIM_ARCH=arm64
export X86SIM_RUNTIME=${A64_RUNTIME:-$X86SIM_ROOT/runtimes/ios-27.0-24A434-arm64/RuntimeRoot}
export X86SIM_DEVICE=${A64_DEVICE:-$X86SIM_ROOT/var/device-a64}
export X86SIM_STATE=${A64_STATE:-$X86SIM_ROOT/var/run-a64}
. "$X86SIM_ROOT/cli/lib/config.sh"
export DPREFIX=${A64_DPREFIX:-$X86SIM_SCRATCH/a64prefix}
export X86SIM_DPREFIX=$DPREFIX
export X86CODE_METAL=${X86CODE_METAL-0}
export X86SIM_MEM_MAX=${X86SIM_MEM_MAX-12G}
X=$X86SIM_ROOT; cd "$X"
D=$X86SIM_DEVICE; ST=$X86SIM_STATE; SECS=${SECS:-90}
mkdir -p "$D" "$ST"
rm -rf "$D"/data/var/log/*.log
# Speed 1: start this boot's mtlvk when Metal is requested. Own socket + pidfile under var/run-a64.
# Speed 8: A64_MTLVK_SOCKET=<path of a running mtlvk's socket> shares that mtlvk (one per host for several devices; it
# keeps an object table per connection): this boot neither starts nor stops it.
stop_a64_mtlvk() {
	[ -n "${A64_MTLVK_SOCKET:-}" ] && return 0
	local old
	old=$(cat "$ST/mtlvk.pid" 2>/dev/null || true)
	if [ -n "$old" ] && [ -d "/proc/$old" ]; then
		kill "$old" 2>/dev/null || true
		sleep 0.2
		[ -d "/proc/$old" ] && kill -9 "$old" 2>/dev/null || true
	fi
	rm -f "$ST/mtlvk.pid" "$ST/mtlvk.sock"
}
if [ -n "$X86CODE_METAL" ] && [ "$X86CODE_METAL" != 0 ]; then
	# On-disk Metal binary so xpc_bundle can find MTLCompilerService.xpc (see ensure-metal-framework.sh).
	"$X/cli/libexec/arm64/ensure-metal-framework.sh" || {
		echo "boot-a64: ensure-metal-framework failed; pipelines will not compile" >&2
	}
	MTLVK_BIN=${MTLVK:-$X86SIM_MTLVK}
	if [ -n "${A64_MTLVK_SOCKET:-}" ]; then
		if [ -S "$A64_MTLVK_SOCKET" ]; then
			export X86CODE_MTLVK_SOCKET=$A64_MTLVK_SOCKET
			echo "boot-a64: sharing mtlvk at $A64_MTLVK_SOCKET, Metal for: $X86CODE_METAL" >&2
		else
			echo "boot-a64: A64_MTLVK_SOCKET $A64_MTLVK_SOCKET is not a socket: software rendering" >&2
			export X86CODE_METAL=0
		fi
	fi
fi
if [ -n "$X86CODE_METAL" ] && [ "$X86CODE_METAL" != 0 ] && [ -z "${A64_MTLVK_SOCKET:-}" ]; then
	stop_a64_mtlvk
	export X86CODE_MTLVK_SOCKET=$ST/mtlvk.sock
	if [ -x "$MTLVK_BIN" ]; then
		setsid "$MTLVK_BIN" "$ST/mtlvk.sock" > "$ST/mtlvk.log" 2>&1 &
		echo $! > "$ST/mtlvk.pid"
		for i in $(seq 1 50); do [ -S "$ST/mtlvk.sock" ] && break; sleep 0.1; done
	fi
	mpid=$(cat "$ST/mtlvk.pid" 2>/dev/null || true)
	if [ -S "$ST/mtlvk.sock" ] && [ -n "$mpid" ] && [ -d "/proc/$mpid" ]; then
		echo "boot-a64: mtlvk up (pid $mpid), Metal for: $X86CODE_METAL" >&2
	else
		echo "boot-a64: mtlvk did not start ($MTLVK_BIN, see $ST/mtlvk.log): software rendering" >&2
		export X86CODE_METAL=0
		stop_a64_mtlvk
	fi
fi
MIN="com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.notifyd com.apple.configd_sim
	com.apple.runningboardd com.apple.lsd com.apple.containermanagerd com.apple.containermanagerd.system
	com.apple.mobilegestalt.xpc com.apple.UserEventAgent-System com.apple.securityd com.apple.trustd com.apple.syslogd
	com.apple.mobile.installd com.apple.systemstatusd com.apple.distnoted.xpc.daemon com.apple.tccd
	com.apple.fontservicesd com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
	com.apple.cloudd com.apple.diagnosticd com.apple.nsurlsessiond com.apple.appprotectiond"
# x86Code: lean UI set (every arm64 process costs 150-450 MB under the JIT). No PosterBoard: it spawns a flock of
# poster extensions (~450 MB each, 10+ processes, +5 GB) that push SHARED=0 past 13 GB before home. No chronod
# (widget extensions) and no FileProvider. A64_JOBS=full adds PosterBoard + chronod + FileProvider.
# A64_JOBS=slim drops sleepd + usernotificationsd (~400-450 MB RSS each under the JIT); home + one app
# does not need them. Default std keeps them for cold-boot stability (BKS XPC). A64_SLIM=1 (default)
# parks Spotlight / WidgetRenderer before dyld (X86CODE_A64_SLIM_PARK). Without cloudd+diagnosticd
# in the job set, securityd busy-retries those Mach lookups (~1.9 cores idle). Those two stay
# runnable (not parked): a parked MachServices daemon accepts the lookup then hangs clients forever.
# nsurlsessiond: without it cloudd busy-retries getActiveSessionIdentifiers (~70% of a core + huge log).
# appprotectiond (Speed 5): without it SpringBoard's ServerBackedClientPolicyStore retries com.apple.appprotectiond.read
# ~5x/s for the whole boot (on-demand, EnablePressuredExit: idle cost only once looked up).
# openurl: com.x86code.openurl (arm64 build) so simtouch.py app/openurl works on the a64 device.
# InputUI (Speed 12): the keyboard service (LaunchAngel, on demand: started at SpringBoard's first osservice request).
# Without the job SpringBoard's bootstrap of osservice<com.apple.InputUI> fails ~60 times per boot and text fields get no
# keyboard. assistantd (on demand) serves com.apple.assistant.dictation: without it each failed dictation availability
# check posts "availability changed", the keyboard dock (mic key) re-checks, and the app's main thread spins at 100%
# from the first text field tap, so InputUI's keyboard arbiter times the app out and no keyboard appears.
UI="com.apple.backboardd com.apple.SpringBoard com.apple.sleepd com.apple.TextInput.kbd com.apple.FileCoordination
	com.apple.usernotificationsd com.apple.InputUI com.apple.assistantd"
UI_SLIM="com.apple.backboardd com.apple.SpringBoard com.apple.TextInput.kbd com.apple.FileCoordination com.apple.InputUI
	com.apple.assistantd"
FULLUI="com.apple.PosterBoard com.apple.chronod com.apple.FileProvider"
OUR_JOBS="com.x86code.openurl=host/openurl/arm64/openurl"
case "${A64_JOBS:-std}" in
	min) JOBS="$MIN $OUR_JOBS" ;;
	slim) JOBS="$MIN $UI_SLIM $OUR_JOBS" ;;
	full) JOBS="$MIN $UI $FULLUI $OUR_JOBS" ;;
	*) JOBS="$MIN $UI $OUR_JOBS" ;;
esac
# Speed 10: A64_CLONE_FROM=<device dir> starts a new device (no data/ yet) from a copy of that device's data/, like
# simctl clone. A fresh device's first boot otherwise builds the LaunchServices database (lsd) while SpringBoard's icon
# requests wait on it: ~80 s, and SpringBoard was killed and relaunched at ~90 s (home after ~5 min). The store refers
# to the apps' data containers, so the whole data/ is copied (reflinks), without logs, run state, tmp and sockets.
if [ -n "${A64_CLONE_FROM:-}" ] && [ ! -d "$D/data" ] && [ -d "$A64_CLONE_FROM/data/var/db/lsd" ]; then
	mkdir -p "$D"
	cp -a --reflink=auto "$A64_CLONE_FROM/data" "$D/data" || exit 1
	rm -rf "$D/data/var/log" "$D/data/var/run" "$D/data/tmp" "$D/data/simhid.sock" "$D/data/framebuffer.bgra"
	mkdir -p "$D/data/var/log" "$D/data/var/run" "$D/data/tmp"
	echo "  cloned data/ from $A64_CLONE_FROM"
fi
X86CODE_JOBLOG=1 python3 cli/libexec/mksimdevice.py "$D" $JOBS "$@" > /dev/null || exit 1
# Speed 8: the container gets a private /dev/shm (X86CODE_PRIVATE_SHM, darlingserver): its remount no longer stacks a
# tmpfs over the host's /dev/shm, and two devices' shm objects stay apart. The frame ring is written to
# /run/user/<uid> (sim.env SIMFB_RING); $SIMFB_RING_LINUX (default /dev/shm/x86code-a64-simfb.ring) links to it.
export X86CODE_PRIVATE_SHM=1
RING=$(sed -n 's#^SIMFB_RING=/Volumes/SystemRoot##p' "$D/sim.env")
if [ -n "$RING" ] && [ -n "${SIMFB_RING_LINUX:-}" ] && [ "$RING" != "$SIMFB_RING_LINUX" ]; then
	ln -sfn "$RING" "$SIMFB_RING_LINUX"
fi
# Strip home-screen / Today widgets so SpringBoard does not demand WidgetRenderer (even with park).
# A64_KEEP_WIDGETS=1 leaves IconState alone (full profile / wallpaper experiments).
if [ "${A64_KEEP_WIDGETS:-0}" != 1 ]; then
	python3 - "$D/data/Library/SpringBoard" <<'PY'
import os, sys, plistlib
d = sys.argv[1]
os.makedirs(d, exist_ok=True)

def strip(obj):
	if isinstance(obj, list):
		out = []
		for x in obj:
			if isinstance(x, dict) and (x.get("elementType") == "widget" or "widgetIdentifier" in x):
				continue
			# drop emptied widget stacks / custom placeholders
			if isinstance(x, dict) and x.get("iconType") == "custom" and not x.get("elements") and "widgetIdentifier" not in x and x.get("elementType") != "folder":
				# keep if it still has meaningful keys beyond stack chrome
				keys = set(x) - {"allowsExternalSuggestions","allowsSuggestions","displayIdentifier","elements","gridSize","iconType","uniqueIdentifier"}
				if not keys:
					continue
			sx = strip(x)
			if isinstance(sx, dict) and sx.get("elements") == [] and sx.get("iconType") == "custom":
				continue
			out.append(sx)
		return out
	if isinstance(obj, dict):
		return {k: strip(v) for k, v in obj.items()}
	return obj

# Speed 9: a fresh device has no IconState yet, so SpringBoard wrote its default layout on the first boot, with the
# Maps and Calendar widgets (two blank white tiles: no chronod / WidgetRenderer). Seed the same layout without widgets.
seed = os.path.join(d, "IconState.plist")
if not os.path.exists(seed):
	import uuid
	u = lambda: str(uuid.uuid4()).upper()
	utilities = {"defaultDisplayName": "Utilities", "displayName": "Utilities", "listType": "folder",
		"iconLists": [["com.apple.shortcuts", "com.apple.Passwords"]], "listUniqueIdentifiers": [u()], "uniqueIdentifier": u()}
	layout = {"buttonBar": ["com.apple.mobilesafari", "com.apple.MobileSMS"], "defaultDisplayName": "Folder", "displayName": "Folder",
		"iconLists": [["com.apple.mobilecal", "com.apple.mobileslideshow", "com.apple.Maps", "com.apple.news", "com.apple.Health",
			"com.apple.Passbook", "com.apple.SiriApp", "com.apple.Preferences"],
			["com.apple.Fitness", "com.apple.Bridge", "com.apple.reminders", "com.apple.DocumentsApp", "com.apple.Preview", utilities,
			"com.apple.MobileAddressBook"]],
		"ignored": ["com.apple.TVRemoteApp"], "listUniqueIdentifiers": [u(), u()], "today": [], "uniqueIdentifier": u()}
	with open(seed, "wb") as f:
		plistlib.dump(layout, f)
	print("  seeded a widget-free IconState.plist (fresh device)", flush=True)

for name in ("IconState.plist", "DesiredIconState.plist"):
	path = os.path.join(d, name)
	if not os.path.exists(path):
		continue
	with open(path, "rb") as f:
		data = plistlib.load(f)
	with open(path, "wb") as f:
		plistlib.dump(strip(data), f)
	print(f"  stripped widgets from {name}", flush=True)
PY
fi
UDID=$(sed -n 's/^SIMULATOR_UDID=//p' "$D/sim.env")
L=$DPREFIX/private/var/tmp/com.apple.CoreSimulator.SimDevice.$UDID/launchd.log
rm -f "$L" "$L".1 "$L".2
R="/Volumes/SystemRoot$X86SIM_RUNTIME"
# Default slim park list: path substrings matched in arm64host before dyld. A64_SLIM=0 clears it.
# Do not park MachServices daemons (cloudd, diagnosticd): lookup succeeds then clients block forever
# on the unreplied port. Those are in MIN so securityd's lookups resolve to real (lean) processes.
PARK_LIST='Spotlight.app,WidgetRenderer'
if [ "${A64_SLIM:-1}" = 1 ] && [ "${A64_JOBS:-std}" != full ]; then
	if grep -q '^X86CODE_A64_SLIM_PARK=' "$D/sim.env" 2>/dev/null; then
		sed -i "s|^X86CODE_A64_SLIM_PARK=.*|X86CODE_A64_SLIM_PARK=$PARK_LIST|" "$D/sim.env"
	else
		echo "X86CODE_A64_SLIM_PARK=$PARK_LIST" >>"$D/sim.env"
	fi
	# File fallback: launchd does not pass sim.env into job environ; arm64host reads this path.
	mkdir -p /home/bobby/scratch/x86code
	echo "$PARK_LIST" > /home/bobby/scratch/x86code/a64-slim-park
else
	rm -f /home/bobby/scratch/x86code/a64-slim-park
fi
ENVS=$(sed 's/^/"/; s/$/"/' "$D/sim.env" | tr '\n' ' ')
ENVS="$ENVS ${A64_ENV:-}"   # A64_ENV: extra K=V for launchd_sim (e.g. X86CODE_A64_TRACE=1)
# stop only this prefix's container: its setuid launchers (timeout can't signal them) are found by their command
# line (this device's path, or tests/arm64/run.sh's a64home), so the x86_64 container's are never touched; then
# darling shutdown and stale state files
stop_prefix() {
	local pids="" d p
	for d in /proc/[0-9]*; do
		p=${d#/proc/}
		[ -r "$d/cmdline" ] || continue
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
[ "${A64_STOP_ONLY:-0}" = 1 ] && exit 0  # Speed 10: stop this device's container only
sleep 1

# Host zone name (no darling yet). The TZ link itself must run *inside* limited.sh so it does not birth
# darlingserver in the caller's cgroup (that was the Stage 2/3 escape: only timeout+darling stayed under MemoryMax).
TZNAME=$(readlink -f /etc/localtime 2>/dev/null | sed -n 's#^.*/zoneinfo/##p')
H="/Volumes/SystemRoot$X/host/simhost/simhost"

# Payload for `darling shell sh -c` — raw text so $ENVS quote tokens are only parsed by that inner sh.
A64_TIMEOUT=$([ "${A64_KEEP:-0}" = 1 ] && echo 86400 || echo $((SECS + 5)))
CMDFILE=$ST/simhost.cmd
cat > "$CMDFILE" <<CMDEOF
exec "$H" /usr/bin/env $ENVS "$R/sbin/launchd_sim" "/Volumes/SystemRoot$D/data/var/run/launchd_bootstrap.plist"
CMDEOF

(cli/libexec/limited.sh env \
	DPREFIX="$DPREFIX" \
	X86SIM_DARLING="$X86SIM_DARLING" \
	A64_TZNAME="$TZNAME" \
	A64_TIMEOUT="$A64_TIMEOUT" \
	A64_CMDFILE="$CMDFILE" \
	A64_PREFETCH_HOT="$([ "${A64_PREFETCH:-1}" != 0 ] && echo "$ST/prefetch.early $ST/prefetch.hot")" \
	"$X/cli/libexec/arm64/boot-inner.sh" > "$ST/lsim.out" 2>&1 &)
# Speed 11: 60 s in (home is up), record which ranges of the dyld cache and the AOT image this boot's processes
# have mapped in; the next boot prefetches them (boot-inner.sh). A64_PREFETCH=0: neither.
if [ "${A64_PREFETCH:-1}" != 0 ]; then
	JC=${X86CODE_A64_JITCACHE_DIR:-$X86SIM_SCRATCH/jitcache}
	# the pages mapped 12 s in are read first (prefetch.early), then the rest: the reads share the boot's IO cap
	# with its own page faults, so the order decides what the first processes wait for
	pf_record() { python3 -I "$X/cli/libexec/arm64/prefetch.py" record "$1" \
		"$X86SIM_RUNTIME"/System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_arm64* \
		"$JC"/*.map "$JC"/*.map.idx; }
	(sleep 12; pf_record "$ST/prefetch.early"; sleep 48; pf_record "$ST/prefetch.hot") > "$ST/prefetch.log" 2>&1 &
fi
sleep 3
(timeout $((SECS + 2)) tail -n +1 -F "$L" 2>/dev/null | grep --line-buffered -v -E "failed lookup|Last log repeated" > "$ST/launchd.log" &)
sleep "$SECS"
echo "== spawns/exits"
grep -E "Successfully spawned|exited due|exited with" "$ST/launchd.log" | sed "s/^[0-9-]* [0-9:.]* //; s/\[[0-9]*\]//g; s/ because.*//; s/, ran for.*//; s/<Notice>: //" | sort | uniq -c | cut -c1-140
echo "== lsim.out (tail)"; tail -5 "$ST/lsim.out"
# Report cgroup containment
echo "== cgroup containment"
# Prefer the running x86sim scope that already holds our guests (highest memory.current). A stale
# scope left from a previous KEEP boot would otherwise win `list-units | head` and report false OUTs.
SCOPE=""
best=-1
for s in $(systemctl --user list-units 'x86sim-*.scope' --no-legend 2>/dev/null | awk '/running/{print $1}'); do
	cg=$(systemctl --user show "$s" -p ControlGroup --value 2>/dev/null)
	cur=$(cat /sys/fs/cgroup$cg/memory.current 2>/dev/null || echo 0)
	if [ "$cur" -gt "$best" ]; then best=$cur; SCOPE=$s; fi
done
if [ -n "$SCOPE" ]; then
	CG=$(systemctl --user show "$SCOPE" -p ControlGroup --value 2>/dev/null)
	echo "scope=$SCOPE memory.current=$(cat /sys/fs/cgroup$CG/memory.current 2>/dev/null) memory.max=$(cat /sys/fs/cgroup$CG/memory.max 2>/dev/null)"
	in=0; out=0; in_rss=0; out_rss=0
	for p in /proc/[0-9]*; do
		[ -r "$p/environ" ] || continue
		grep -qaz X86CODE_A64 "$p/environ" 2>/dev/null || continue
		cg=$(awk -F: '/^0::/{print $NF; exit}' "$p/cgroup" 2>/dev/null)
		rss=$(awk '/^VmRSS/{print $2}' "$p/status" 2>/dev/null); rss=${rss:-0}
		case "$cg" in
			*"$SCOPE"*) in=$((in+1)); in_rss=$((in_rss+rss)) ;;
			*) out=$((out+1)); out_rss=$((out_rss+rss)); echo "  OUT pid=${p#/proc/} rss=$((rss/1024))MB cg=$cg" ;;
		esac
	done
	# darlingserver for this prefix
	for p in /proc/[0-9]*; do
		[ -r "$p/cmdline" ] || continue
		tr '\0' ' ' < "$p/cmdline" 2>/dev/null | grep -q "darlingserver $DPREFIX" || continue
		cg=$(awk -F: '/^0::/{print $NF; exit}' "$p/cgroup" 2>/dev/null)
		rss=$(awk '/^VmRSS/{print $2}' "$p/status" 2>/dev/null); rss=${rss:-0}
		case "$cg" in
			*"$SCOPE"*) echo "  darlingserver IN rss=$((rss/1024))MB" ;;
			*) echo "  darlingserver OUT rss=$((rss/1024))MB cg=$cg"; out=$((out+1)); out_rss=$((out_rss+rss)) ;;
		esac
	done
	echo "guests IN=$in ($((in_rss/1024)) MB) OUT=$out ($((out_rss/1024)) MB)"
fi
# A64_SLIM=1 (default): report parked helpers (X86CODE_A64_SLIM_PARK). The old slim-reaper.sh
# kill loop is gone — SpringBoard holds RunningBoard assertions and relaunches within ~1 s,
# which burned a core in xpcproxy_sim and never saved memory.
if [ "${A64_SLIM:-1}" = 1 ]; then
	echo "== slim park status"
	parked=""
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ -r "$p/comm" ] || continue
		c=$(cat "$p/comm" 2>/dev/null) || continue
		case "$c" in
			Spotlight|WidgetRenderer_*|pkd)
				# launchd does not put SLIM_PARK in job environ; detect via pause + tiny RSS
				w=$(cat "$p/wchan" 2>/dev/null || true)
				r=$(awk '/^VmRSS/{print $2}' "$p/status" 2>/dev/null); r=${r:-0}
				case "$w" in
					*pause*)
						echo "  $c pid=$pid rss_mb=$((r/1024)) (parked)"
						parked="$parked $c"
						;;
					*)
						[ "$r" -lt 51200 ] && echo "  $c pid=$pid rss_mb=$((r/1024)) wchan=$w"
						;;
				esac
				;;
		esac
	done
	echo "  parked:$parked"
	# Ensure no leftover reaper from an older boot
	if [ -f "$ST/slim-reaper.pid" ]; then
		old=$(cat "$ST/slim-reaper.pid" 2>/dev/null || true)
		[ -n "$old" ] && [ -d "/proc/$old" ] && kill "$old" 2>/dev/null || true
		rm -f "$ST/slim-reaper.pid"
	fi
fi
# A64_KEEP=1: leave the device running (the container outlives the launcher's timeout); else stop this prefix
if [ "${A64_KEEP:-0}" = 1 ]; then
	:
else
	stop_prefix
	stop_a64_mtlvk
fi
