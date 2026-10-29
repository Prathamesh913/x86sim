#!/bin/sh
# Builds libarm64jit.so (Linux) and host/arm64host (x86_64 Mach-O) and installs both next to Darling's mldr, which
# runs arm64-only Mach-Os through them (arm64 plan, stage 1). Files are replaced by rename (running processes keep the
# old ones). Usage: linux/arm64jit/install.sh   (build dir: $X86SIM_SCRATCH/arm64/build-arm64jit)
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
set -e
B=${ARM64JIT_BUILD:-$X86SIM_SCRATCH/arm64/build-arm64jit}
mkdir -p "$B"
[ -f "$B/build.ninja" ] || cmake -S "$X86SIM_ROOT/linux/arm64jit" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > "$B/cmake.log"
nice ninja -C "$B" -j"${JOBS:-6}" arm64jit
"$X86SIM_ROOT/host/arm64host/build.sh" "$X86SIM_DARLING_BUILD"
D=$X86SIM_PREFIX/libexec/darling/usr/libexec/darling
for f in "$B/libarm64jit.so" "$X86SIM_ROOT/host/arm64host/arm64host"; do
	cp "$f" "$D/.$(basename "$f").new" && mv -f "$D/.$(basename "$f").new" "$D/$(basename "$f")"
done
echo "installed arm64host and libarm64jit.so in $D"
