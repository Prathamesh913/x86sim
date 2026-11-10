#!/bin/bash
# x86Sim: build the test apps (tests/apps/<name>) for the arm64 iOS Simulator on the Mac (X86SIM_MAC) with swiftc —
# no Xcode project — and copy the .app bundles to $X86SIM_SCRATCH/apps/. Usage: tests/apps/build.sh [name...]
# On Linux (X86SIM_BUILD=linux, build/mac-env.sh: the default when sdks/iPhoneSimulator.sdk exists) the same commands
# run here: build/linux-bin's xcrun/codesign map to build/linux-swiftc.sh + ld64.lld.
set -e
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
. "$X86SIM_ROOT/build/mac-env.sh"
A=$X86SIM_ROOT/tests/apps; OUT=$X86SIM_SCRATCH/apps; mkdir -p "$OUT"
names=${*:-$(cd "$A" && ls -d x86*/ | tr -d /)}
for n in $names; do
	echo "== $n" >&2
	tar cf - -C "$A" common.swift "$n" | mac "set -e; W=\$(mktemp -d /tmp/x86app.XXXXXX); cd \$W; tar xf -
		APP=$n.app; mkdir -p \$APP
		nice xcrun -sdk iphonesimulator swiftc -target arm64-apple-ios27.0-simulator -Onone \$( [ -f $n/main.swift ] || echo -parse-as-library ) \
			-o \$APP/$n common.swift $n/*.swift >&2
		cat > \$APP/Info.plist <<PL
<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\"><dict>
<key>CFBundleIdentifier</key><string>com.x86sim.test.$n</string>
<key>CFBundleName</key><string>$n</string><key>CFBundleDisplayName</key><string>$n</string>
<key>CFBundleExecutable</key><string>$n</string><key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string><key>CFBundleVersion</key><string>1</string>
<key>CFBundleSupportedPlatforms</key><array><string>iPhoneSimulator</string></array>
<key>DTPlatformName</key><string>iphonesimulator</string><key>MinimumOSVersion</key><string>27.0</string>
<key>UIDeviceFamily</key><array><integer>1</integer></array><key>UILaunchScreen</key><dict/>
<key>UIApplicationSceneManifest</key><dict><key>UIApplicationSupportsMultipleScenes</key><false/></dict>
<key>NSAppTransportSecurity</key><dict><key>NSAllowsArbitraryLoads</key><true/></dict>
</dict></plist>
PL
		codesign -f -s - \$APP >&2
		tar cf - \$APP; cd /; rm -rf \$W" | (cd "$OUT" && rm -rf "$n.app" && tar xf -)
	ls -d "$OUT/$n.app"
done
