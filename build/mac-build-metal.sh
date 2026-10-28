#!/bin/sh
# Build host/metalhostd/metalhostd (Darling-side Metal service: metalhostd.m + vkmtl.m) and tests/metal/mtlprobe
# (test client) as x86_64 iOS-simulator binaries on the Mac ($X86SIM_MAC).
set -e
. "$(dirname "$0")/mac-env.sh"
CC="$SIMCC -O1 -g -Wall -Wno-deprecated-declarations -Wno-unguarded-availability-new"
W=$(mktemp -d "$X86SIM_SCRATCH/macbuild.XXXXXX")
tar cf - -C "$X/host/metalhostd" vkmtl.m vkmtl_proto.h metalhostd.m -C "$X/tests/metal" mtlprobe.m shaders/probe_metallib.h shaders/probe_source.h | mac "
rm -rf /tmp/vkm && mkdir -p /tmp/vkm && cd /tmp/vkm && tar xf - &&
$CC -fno-objc-arc -framework Foundation -framework Metal -framework IOSurface -lobjc -o metalhostd metalhostd.m vkmtl.m >&2 &&
$CC -fobjc-arc -framework Foundation -framework Metal -framework IOSurface -o mtlprobe mtlprobe.m >&2 &&
tar cf - metalhostd mtlprobe" | tar xf - -C "$W"
# rename over the old binaries (a running simulator may have them mapped)
mv "$W/metalhostd" "$X/host/metalhostd/metalhostd"; mv "$W/mtlprobe" "$X/tests/metal/mtlprobe"; rmdir "$W"
ls -la "$X/host/metalhostd/metalhostd" "$X/tests/metal/mtlprobe"
