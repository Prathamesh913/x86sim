#!/bin/bash
# ARM64-PLAN stage 2 / Speed 1: arm64 iOS-simulator builds of the shims loaded into simulator processes (they call
# the guest's frameworks, so they must be guest code): simshim, SimFramebuffer (simfb), SimulatorHID (simhid),
# metalhostd (+vkmtl), iosurfaced, mtlprobe. Output under host/<name>/arm64/ (mksimdevice.py X86SIM_ARCH=arm64
# picks them up). The x86_64 builds are not touched.
# Usage: build/mac-build-arm64-shims.sh [simshim simaudio simfb simhid metalhostd iosurfaced mtlprobe openurl netconfig]
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
simaudio)
	# Speed 27: the audio devices (CoreAudio HAL stand-in, host/simaudio) for arm64 processes; as build/mac-build-simaudio.sh:
	# flat namespace + dynamic lookup, CoreAudio references weak (inert in processes without CoreAudio)
	mac "rm -rf /tmp/a64sa && mkdir -p /tmp/a64sa && cat > /tmp/a64sa/simaudio.c && cd /tmp/a64sa && xcrun -sdk iphonesimulator clang $T \
		-O2 -Wall -Wno-unused-function -dynamiclib -framework CoreFoundation -Wl,-flat_namespace -Wl,-undefined,dynamic_lookup \
		-o libsimaudio.dylib simaudio.c >&2 && cat libsimaudio.dylib" < "$X/host/simaudio/simaudio.c" > "$W/libsimaudio.dylib"
	mkdir -p "$X/host/simaudio/arm64" && mv "$W/libsimaudio.dylib" "$X/host/simaudio/arm64/libsimaudio.dylib" ;;
simfb)
	mac "rm -rf /tmp/a64fb && mkdir -p /tmp/a64fb && cat > /tmp/a64fb/simfb.c && cd /tmp/a64fb && xcrun -sdk iphonesimulator clang $T \
		-DWIDTH0=1206 -DHEIGHT0=2622 -O1 -Wall -Wno-unused-parameter -fvisibility=hidden -dynamiclib \
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
metalhostd)
	# Same sources as build/mac-build-metal.sh, arm64 sim target. Runs under the A64 JIT; talks to native mtlvk.
	tar cf - -C "$X/host/metalhostd" vkmtl.m vkmtl_proto.h metalhostd.m | mac "
	rm -rf /tmp/a64mtl && mkdir -p /tmp/a64mtl && cd /tmp/a64mtl && tar xf - &&
	xcrun -sdk iphonesimulator clang $T -O1 -g -Wall -Wno-deprecated-declarations -Wno-unguarded-availability-new \
		-fno-objc-arc -framework Foundation -framework Metal -framework IOSurface -lobjc \
		-o metalhostd metalhostd.m vkmtl.m >&2 &&
	cat metalhostd" > "$W/metalhostd"
	mkdir -p "$X/host/metalhostd/arm64" && mv "$W/metalhostd" "$X/host/metalhostd/arm64/metalhostd"
	chmod +x "$X/host/metalhostd/arm64/metalhostd" ;;
iosurfaced)
	# Same link as build/mac-build-iosurface.sh: Foundation + IOKit (IOCFSerialize), not IOSurface.framework.
	tar cf - -C "$X/host/iosurfaced" iosurfaced.m | mac "
	rm -rf /tmp/a64ios && mkdir -p /tmp/a64ios && cd /tmp/a64ios && tar xf - &&
	xcrun -sdk iphonesimulator clang $T -O1 -g -Wall -Wno-deprecated-declarations -Wno-unguarded-availability-new \
		-fno-objc-arc -framework Foundation -framework IOKit -lobjc \
		-o iosurfaced iosurfaced.m >&2 &&
	cat iosurfaced" > "$W/iosurfaced"
	mkdir -p "$X/host/iosurfaced/arm64" && mv "$W/iosurfaced" "$X/host/iosurfaced/arm64/iosurfaced"
	chmod +x "$X/host/iosurfaced/arm64/iosurfaced" ;;
mtlprobe)
	tar cf - -C "$X/tests/metal" mtlprobe.m shaders/probe_metallib.h shaders/probe_source.h | mac "
	rm -rf /tmp/a64probe && mkdir -p /tmp/a64probe && cd /tmp/a64probe && tar xf - &&
	xcrun -sdk iphonesimulator clang $T -O1 -g -Wall -Wno-deprecated-declarations -Wno-unguarded-availability-new \
		-fobjc-arc -framework Foundation -framework Metal -framework IOSurface \
		-o mtlprobe mtlprobe.m >&2 &&
	cat mtlprobe" > "$W/mtlprobe"
	mkdir -p "$X/tests/metal/arm64" && mv "$W/mtlprobe" "$X/tests/metal/arm64/mtlprobe"
	chmod +x "$X/tests/metal/arm64/mtlprobe" ;;
openurl)
	# Same sources as build/mac-build-jobs.sh; arm64 sim target. Linux inotify via svc #0xfffe.
	tar cf - -C "$X/host/openurl" openurl.m openurl.entitlements | mac "
	rm -rf /tmp/a64openurl && mkdir -p /tmp/a64openurl && cd /tmp/a64openurl && tar xf - &&
	xcrun -sdk iphonesimulator clang $T -O1 -g -Wall -Wno-deprecated-declarations \
		-fobjc-arc -framework Foundation -Wl,-sectcreate,__TEXT,__entitlements,openurl.entitlements \
		-o openurl openurl.m >&2 &&
	cat openurl" > "$W/openurl"
	mkdir -p "$X/host/openurl/arm64" && mv "$W/openurl" "$X/host/openurl/arm64/openurl"
	chmod +x "$X/host/openurl/arm64/openurl" ;;
netconfig)
	# Speed 26: the host network configuration into com.x86code.host-configd (mksimdevice skips the job when this build
	# is missing: the arm64 phone then had no network state and every request failed "offline", -1009)
	tar cf - -C "$X/host/netconfig" netconfig.m | mac "
	rm -rf /tmp/a64netcfg && mkdir -p /tmp/a64netcfg && cd /tmp/a64netcfg && tar xf - &&
	xcrun -sdk iphonesimulator clang $T -O1 -g -Wall -Wno-deprecated-declarations \
		-fobjc-arc -framework Foundation -framework SystemConfiguration -o netconfig netconfig.m >&2 &&
	cat netconfig" > "$W/netconfig"
	mkdir -p "$X/host/netconfig/arm64" && mv "$W/netconfig" "$X/host/netconfig/arm64/netconfig"
	chmod +x "$X/host/netconfig/arm64/netconfig" ;;
*) echo "unknown target: $w" >&2; exit 1 ;;
esac; done
file "$X"/host/sim*/arm64/*/* "$X"/host/sim*/arm64/*/*/* \
	"$X"/host/metalhostd/arm64/metalhostd "$X"/host/iosurfaced/arm64/iosurfaced \
	"$X"/tests/metal/arm64/mtlprobe "$X"/host/openurl/arm64/openurl "$X"/host/netconfig/arm64/netconfig 2>/dev/null | grep -i mach || true
