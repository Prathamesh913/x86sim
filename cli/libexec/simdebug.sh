#!/bin/bash
# x86Code: `x86sim debug`: LLDB on Linux attached to a simulator app, through LLVM's debugserver running inside the
# Darling container (host/debugserver/debugserver, built by `make mac-debugserver`). debugserver is a macOS x86_64
# process there and uses Darling's Mach task/thread/exception calls; Linux's lldb (with its Mach-O and gdb-remote
# support) connects to it over TCP and reads the Mach-O files from the Linux paths (/Volumes/SystemRoot -> /).
#   x86sim debug [--launch] [--port N] [--server-only] <bundle id | pid> [-- lldb args...]
#     --launch       (re)launch the app waiting for the debugger (x86sim launch -w --terminate-running-process)
#                    and attach before its first instruction of main; default: attach to the running app (or launch
#                    it waiting if it isn't running)
#     --server-only  only start debugserver and print the lldb commands (connect from another terminal or an IDE)
. "$(dirname "$0")/../lib/config.sh"
L=$(dirname "$0")
port=12345 launch=0 server=0
while [ $# -gt 0 ]; do
	case $1 in
		--launch) launch=1 ;;
		--port) port=$2; shift ;;
		--server-only) server=1 ;;
		-*) echo "x86sim debug: unknown option $1" >&2; exit 2 ;;
		*) break ;;
	esac
	shift
done
[ $# -ge 1 ] || { sed -n 2,11p "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
target=$1; shift
[ "${1:-}" = -- ] && shift
DS=$X86SIM_ROOT/host/debugserver/debugserver
[ -x "$DS" ] || { echo "x86sim debug: $DS is not built (make mac-debugserver)" >&2; exit 1; }
command -v lldb > /dev/null || [ $server = 1 ] || { echo "x86sim debug: no lldb on PATH (install your distribution's lldb)" >&2; exit 1; }
exe=
if [[ $target =~ ^[0-9]+$ ]]; then
	pid=$target
else
	exe=$(python3 "$L/simctl.py" get_app_container "$target" app)/$(python3 "$L/simctl.py" listapps --json | python3 -c 'import json,sys; print(json.load(sys.stdin)[sys.argv[1]]["CFBundleExecutable"])' "$target") || exit 1
	pid=
	[ $launch = 0 ] && pid=$(python3 "$L/simctl.py" pidof "$target" 2>/dev/null)
	if [ -z "$pid" ]; then
		pid=$(python3 "$L/simctl.py" launch -w --terminate-running-process "$target" | awk '{print $2}')
		[ -n "$pid" ] && [ "$pid" -gt 0 ] || { echo "x86sim debug: launching $target failed" >&2; exit 1; }
	fi
fi
log=$X86SIM_SCRATCH/debugserver-$pid.log
# debugserver inside the simulator's container (darling shell joins the running one); it exits when lldb detaches
setsid "$X86SIM_DARLING" shell "$X86SIM_DVIEW$DS" "127.0.0.1:$port" --attach="$pid" > "$log" 2>&1 < /dev/null &
for _ in $(seq 120); do
	ss -ltn 2>/dev/null | grep -q "127.0.0.1:$port " && break
	grep -q 'error' "$log" && { cat "$log" >&2; exit 1; }
	sleep 0.5
done
ss -ltn | grep -q "127.0.0.1:$port " || { echo "x86sim debug: debugserver did not start (see $log)" >&2; exit 1; }
cmds=(-o "platform select remote-macosx")
[ -n "$exe" ] && cmds+=(-o "target create --arch x86_64-apple-ios-simulator \"$exe\"")
cmds+=(-o "image search-paths add $X86SIM_DVIEW /" -o "process connect connect://127.0.0.1:$port")
echo "x86sim debug: debugserver attached to pid $pid on 127.0.0.1:$port (log $log)" >&2
if [ $server = 1 ]; then
	printf 'lldb'; printf " %q" "${cmds[@]}"; echo
	exit 0
fi
exec lldb "${cmds[@]}" "$@"
