#!/bin/sh
# Usage: run-sim.sh <linux path to iossimulator binary> [args...]
# Runs an iOS-simulator Mach-O under Darling with dyld_sim from the extracted runtime.
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
R="/Volumes/SystemRoot$X86SIM_RUNTIME"
bin=$(realpath "$1"); shift
exec timeout "${TIMEOUT:-60}" "$X86SIM_DARLING" shell env DYLD_ROOT_PATH="$R" SIMULATOR_ROOT="$R" IPHONE_SIMULATOR_ROOT="$R" DYLD_INSERT_LIBRARIES="/Volumes/SystemRoot$X86SIM_PREFIX/libexec/darling/usr/lib/darling/libkqueue_sim.dylib" $SIMENV "/Volumes/SystemRoot$bin" "$@" < /dev/null
