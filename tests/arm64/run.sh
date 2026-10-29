#!/bin/sh
# Runs an arm64 iOS-simulator binary inside Darling (mldr -> host/arm64host -> libarm64jit, dyld_sim and the prebuilt
# shared cache of the iOS 27 runtime). Uses its own Darling prefix (A64_DPREFIX, default $X86SIM_SCRATCH/a64prefix) so a
# booted x86 simulator's container is left alone.
# Usage: tests/arm64/run.sh <linux path to binary> [args...]   (X86CODE_A64_STATS=1, X86CODE_A64_TRACE=1 pass through)
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
R=/Volumes/SystemRoot${A64_RUNTIME:-$X86SIM_ROOT/runtimes/ios-27.0-24A434-arm64/RuntimeRoot}
export DPREFIX="${A64_DPREFIX:-$X86SIM_SCRATCH/a64prefix}"
bin=$(realpath "$1"); shift
exec timeout "${TIMEOUT:-120}" "$X86SIM_DARLING" shell env LIBC_FATAL_STDERR_=1 DYLD_ROOT_PATH="$R" SIMULATOR_ROOT="$R" IPHONE_SIMULATOR_ROOT="$R" SIMULATOR_SHARED_RESOURCES_DIRECTORY=/Volumes/SystemRoot$X86SIM_SCRATCH/a64home HOME=/Volumes/SystemRoot$X86SIM_SCRATCH/a64home \
	${X86CODE_A64_STATS:+X86CODE_A64_STATS=1} ${X86CODE_A64_TRACE:+X86CODE_A64_TRACE=1} ${X86CODE_A64_STEP:+X86CODE_A64_STEP=1} ${X86CODE_A64_RSS:+X86CODE_A64_RSS=1} \
	${X86CODE_A64_SHARED:+X86CODE_A64_SHARED=$X86CODE_A64_SHARED} ${X86CODE_A64_CODECACHE_MB:+X86CODE_A64_CODECACHE_MB=$X86CODE_A64_CODECACHE_MB} \
	${X86CODE_A64_LINK:+X86CODE_A64_LINK=$X86CODE_A64_LINK} ${X86CODE_A64_SAFE:+X86CODE_A64_SAFE=$X86CODE_A64_SAFE} \
	${X86CODE_A64_PROFILE:+X86CODE_A64_PROFILE=1} "/Volumes/SystemRoot$bin" "$@" < /dev/null
