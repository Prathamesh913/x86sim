#!/bin/sh
# x86Code: build host/iosurfaced/iosurfaced (com.apple.IOSurface.Remote service) and tests/iosurface/iosprobe
# (two-process test) as x86_64 iOS-simulator binaries on the Mac ($X86SIM_MAC).
set -e
. "$(dirname "$0")/mac-env.sh"
CC="$SIMCC -O1 -g -Wall -Wno-deprecated-declarations -Wno-unguarded-availability-new"
W=$(mktemp -d "$X86SIM_SCRATCH/macbuild.XXXXXX")
tar cf - -C "$X/host/iosurfaced" iosurfaced.m -C "$X/tests/iosurface" iosprobe.m | mac "
rm -rf /tmp/ios5 && mkdir -p /tmp/ios5 && cd /tmp/ios5 && tar xf - &&
$CC -fno-objc-arc -framework Foundation -framework IOKit -o iosurfaced iosurfaced.m >&2 &&
$CC -fobjc-arc -framework Foundation -framework IOSurface -o iosprobe iosprobe.m >&2 &&
tar cf - iosurfaced iosprobe" | tar xf - -C "$W"
mv "$W/iosurfaced" "$X/host/iosurfaced/iosurfaced"; mv "$W/iosprobe" "$X/tests/iosurface/iosprobe"; rmdir "$W"
ls -la "$X/host/iosurfaced/iosurfaced" "$X/tests/iosurface/iosprobe"
