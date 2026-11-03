#!/bin/bash
# x86Code: iOS 27 ships Metal.framework's executable only in the dyld shared cache.
# On-disk tree has Info.plist, metallibs, and XPCServices/MTLCompilerService.xpc, but no Metal binary.
# Without the on-disk executable, CFBundle / xpc_bundle cannot resolve the framework path for the
# embedded Application XPC service, so MTLCompilerService lookups fail with "No such process" (error 3).
# A LaunchDaemon for the same label breaks multi-instance create-copy (error 137).
# Fix: extract Metal from the cache into the framework (dyld still prefers the cache; this is for bundle paths).
# Never commit the extracted binary (runtimes/ is gitignored).
set -e
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}
RT=${A64_RUNTIME:-${X86SIM_RUNTIME:-$X86SIM_ROOT/runtimes/ios-27.0-24A434-arm64/RuntimeRoot}}
MFW=$RT/System/Library/Frameworks/Metal.framework
CACHE=$RT/System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_arm64
OUT=$MFW/Metal
if [ -x "$OUT" ] && [ -s "$OUT" ]; then
	echo "ensure-metal-framework: $OUT already present ($(stat -c%s "$OUT") bytes)" >&2
	exit 0
fi
if [ ! -f "$CACHE" ]; then
	echo "ensure-metal-framework: missing cache $CACHE" >&2
	exit 1
fi
if ! command -v ipsw >/dev/null 2>&1; then
	echo "ensure-metal-framework: need ipsw (dyld extract)" >&2
	exit 1
fi
TMP=${X86SIM_SCRATCH:-$HOME/scratch/x86code}/metal-framework-extract
rm -rf "$TMP"
mkdir -p "$TMP"
echo "ensure-metal-framework: extracting Metal from dyld cache..." >&2
ipsw dyld extract "$CACHE" Metal -o "$TMP"
if [ ! -f "$TMP/Metal" ]; then
	echo "ensure-metal-framework: extract produced no Metal binary" >&2
	exit 1
fi
cp "$TMP/Metal" "$OUT"
chmod +x "$OUT"
echo "ensure-metal-framework: installed $OUT ($(stat -c%s "$OUT") bytes)" >&2
