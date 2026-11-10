#!/bin/sh
# x86Sim: build the optional diagnostic shims host/simshim/diag/arm64/lib<name>.dylib (arm64 iOS simulator) on the Mac
# ($X86SIM_MAC). Use one with X86SIM_INSERT=host/simshim/diag/arm64/lib<name>.dylib on a boot (see each file's header).
#   build/mac-build-diag.sh [name ...]     (default: all; name = source file without extension, e.g. syncdiag)
# X86SIM_DATA_DIR (their log directory) is the device's data directory as seen inside Darling.
# X86SIM_DEVICE: the phone whose data directory gets the logs.
set -e
. "$(dirname "$0")/mac-env.sh"
cd "$X/host/simshim/diag"
DATA="/Volumes/SystemRoot$X86SIM_DEVICE/data"
names=${*:-$(ls *.c *.m | sed 's/\.[cm]$//')}
OUT=arm64; mkdir -p arm64
for n in $names; do
	src=$(ls $n.c $n.m 2>/dev/null | head -1)
	[ -n "$src" ] || { echo "no source for $n" >&2; exit 1; }
	# Objective-C shims: ARC where they compile with it, else manual reference counting
	case $src in *.m) fw="-framework Foundation -lobjc" ;; *) fw="-framework CoreFoundation" ;; esac
	CC="$SIMCC -O1 -Wall -Wno-unused-function -Wno-deprecated-declarations -dynamiclib -undefined dynamic_lookup $fw -DX86SIM_DATA_DIR='\"$DATA\"' -o lib$n.dylib $src"
	tar cf - "$src" diagpath.h | mac "rm -rf /tmp/x86diag && mkdir -p /tmp/x86diag && cd /tmp/x86diag && tar xf - &&
		{ $CC -fobjc-arc 2>/dev/null || $CC -fno-objc-arc >&2; } && cat lib$n.dylib" > $OUT/lib$n.dylib.new
	mv $OUT/lib$n.dylib.new $OUT/lib$n.dylib; echo "built $OUT/lib$n.dylib"
done
