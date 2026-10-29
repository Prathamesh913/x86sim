#!/bin/bash
# ARM64-PLAN stage 2: arm64 iOS-simulator builds of the shims loaded into simulator processes (they call the guest's
# frameworks, so they must be guest code): simshim, SimFramebuffer (simfb), SimulatorHID (simhid). Output next to the
# x86_64 builds under host/<name>/arm64/ (mksimdevice.py X86SIM_ARCH=arm64 picks them up). The x86_64 builds are not
# touched. Usage: build/mac-build-arm64-shims.sh [simshim simfb simhid]
set -e
. "$(dirname "$0")/mac-env.sh"
T="-target arm64-apple-ios17.0-simulator"
W=$(mktemp -d "$X86SIM_SCRATCH/macbuild.XXXXXX"); trap 'rm -rf "$W"' EXIT
what=${*:-simshim simfb simhid}
for w in $what; do case $w in
simshim)
	tar cf - -C "$X/host/simshim/compat" simshim.c -C "$X/host/simshim/diag" oslogdiag.m diagpath.h | mac "
	rm -rf /tmp/a64sh && mkdir -p /tmp/a64sh && cd /tmp/a64sh && tar xf - &&
	cc() { xcrun -sdk iphonesimulator clang $T -O1 -Wall -Wno-unused-variable -Wno-deprecated-declarations \"\$@\"; } &&
	cc -c -o simshim.o simshim.c >&2 && cc -c -DSIMSHIM_OSLOG -o oslog.o oslogdiag.m >&2 &&
	cc -dynamiclib -framework IOKit -framework CoreFoundation -lobjc -lsqlite3 -o libsimshim.dylib simshim.o oslog.o >&2 &&
	cat libsimshim.dylib" > "$W/libsimshim.dylib"
	mkdir -p "$X/host/simshim/arm64/compat" && mv "$W/libsimshim.dylib" "$X/host/simshim/arm64/compat/libsimshim.dylib" ;;
simfb)
	mac "rm -rf /tmp/a64fb && mkdir -p /tmp/a64fb && cat > /tmp/a64fb/simfb.c && cd /tmp/a64fb && xcrun -sdk iphonesimulator clang $T \
		-DWIDTH=1206 -DHEIGHT=2622 -O1 -Wall -Wno-unused-parameter -fvisibility=hidden -dynamiclib \
		-install_name /System/Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer -framework CoreFoundation \
		-framework CoreGraphics -framework IOSurface -lobjc -o SimFramebuffer simfb.c >&2 && cat SimFramebuffer" \
		< "$X/host/simfb/simfb.c" > "$W/SimFramebuffer"
	mkdir -p "$X/host/simfb/arm64/SimFramebuffer.framework" && mv "$W/SimFramebuffer" "$X/host/simfb/arm64/SimFramebuffer.framework/" ;;
simhid)
	mac "rm -rf /tmp/a64hid && mkdir -p /tmp/a64hid && cat > /tmp/a64hid/simhid.m && cd /tmp/a64hid && xcrun -sdk iphonesimulator clang $T \
		-O1 -Wall -Wno-unused-parameter -fno-objc-arc -dynamiclib -install_name /System/Library/PrivateFrameworks/SimulatorHID.framework/SimulatorHID \
		-framework Foundation -framework IOKit -lobjc -o SimulatorHID simhid.m >&2 && cat SimulatorHID" < "$X/host/simhid/simhid.m" > "$W/SimulatorHID"
	mkdir -p "$X/host/simhid/arm64/SimulatorHID.framework" && mv "$W/SimulatorHID" "$X/host/simhid/arm64/SimulatorHID.framework/"
	cp "$X/host/simhid/SimulatorHID.framework/Info.plist" "$X/host/simhid/arm64/SimulatorHID.framework/" ;;
esac; done
file "$X"/host/sim*/arm64/*/* "$X"/host/sim*/arm64/*/*/* 2>/dev/null | grep -i mach
