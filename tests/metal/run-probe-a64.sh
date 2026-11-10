#!/bin/bash
# x86Sim Speed 1: arm64 Metal probe on the iOS 27 runtime (own prefix/device/state; does not touch the x86 stack).
# Needs host/metalhostd/arm64/metalhostd, tests/metal/arm64/mtlprobe, host/iosurfaced/arm64/iosurfaced
# (build/mac-build-arm64-shims.sh metalhostd iosurfaced mtlprobe). Starts mtlvk on var/run-a64-probe/.
# Usage: SECS=60 tests/metal/run-probe-a64.sh
set -e
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
. "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT; cd "$X"
export X86SIM_ARCH=arm64
export X86SIM_RUNTIME=${A64_RUNTIME:-$X/runtimes/ios-27.2-24B8117-arm64/RuntimeRoot}
export X86SIM_DEVICE=${A64_PROBE_DEVICE:-$X/var/device-a64-probe}
export X86SIM_STATE=${A64_PROBE_STATE:-$X/var/run-a64-probe}
export DPREFIX=${A64_PROBE_DPREFIX:-$X86SIM_SCRATCH/a64probeprefix}
export X86SIM_DPREFIX=$DPREFIX
export X86SIM_METAL=1
export X86SIM_IOSURFACE=1
export X86SIM_VKMTL_LOG=${X86SIM_VKMTL_LOG:-1}
export X86SIM_MEM_MAX=${X86SIM_MEM_MAX-6G}
D=$X86SIM_DEVICE; ST=$X86SIM_STATE
mkdir -p "$D" "$ST"
rm -rf "$D"/data/var/log/*.log
MTLVK_BIN=${MTLVK:-$X86SIM_MTLVK}
SOCK=$ST/mtlvk.sock
export X86SIM_MTLVK_SOCKET=$SOCK
# Stop only our previous probe mtlvk (pidfile), never pkill -x
if [ -f "$ST/mtlvk.pid" ]; then
	old=$(cat "$ST/mtlvk.pid")
	[ -n "$old" ] && [ -d "/proc/$old" ] && kill "$old" 2>/dev/null || true
	rm -f "$ST/mtlvk.pid"
fi
rm -f "$SOCK"
if [ ! -x "$X/host/metalhostd/arm64/metalhostd" ] || [ ! -x "$X/tests/metal/arm64/mtlprobe" ]; then
	echo "missing arm64 metalhostd/mtlprobe; run: build/mac-build-arm64-shims.sh metalhostd iosurfaced mtlprobe" >&2
	exit 1
fi
setsid "$MTLVK_BIN" "$SOCK" > "$ST/mtlvk.log" 2>&1 &
echo $! > "$ST/mtlvk.pid"
for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
if [ ! -S "$SOCK" ]; then
	echo "mtlvk failed to start; see $ST/mtlvk.log" >&2
	exit 1
fi
echo "mtlvk up pid=$(cat $ST/mtlvk.pid) sock=$SOCK"
# Minimal boot via boot-a64 (rebuilds jobs; starts mtlvk on this ST). mksimdevice adds metal/iosurface from env.
export A64_DEVICE=$D A64_STATE=$ST A64_DPREFIX=$DPREFIX A64_RUNTIME=$X86SIM_RUNTIME
export A64_JOBS=min A64_SLIM=0 A64_KEEP=0 A64_KEEP_WIDGETS=1
SECS=${SECS:-55} X86SIM_METAL=1 \
	cli/libexec/arm64/boot-a64.sh "com.x86sim.mtlprobe=$X/tests/metal/arm64/mtlprobe" || true
echo "=== mtlprobe"; cat "$D/data/var/log/com.x86sim.mtlprobe.log" 2>/dev/null | tail -80
echo "=== metalhostd"; tail -40 "$D/data/var/log/com.x86sim.metalhostd.log" 2>/dev/null
echo "=== iosurfaced"; tail -20 "$D/data/var/log/com.x86sim.iosurfaced.log" 2>/dev/null
echo "=== mtlvk"; grep -v 'metal2vulkan-' "$ST/mtlvk.log" 2>/dev/null | tail -30
# cleanup probe mtlvk
if [ -f "$ST/mtlvk.pid" ]; then
	kill "$(cat "$ST/mtlvk.pid")" 2>/dev/null || true
	rm -f "$ST/mtlvk.pid" "$SOCK"
fi
