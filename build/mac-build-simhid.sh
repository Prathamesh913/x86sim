#!/bin/sh
# Build host/simhid/SimulatorHID.framework (x86_64 iOS simulator stand-in for CoreSimulator's SimulatorHID) on the Mac ($X86SIM_MAC)
set -e
. "$(dirname "$0")/mac-env.sh"
cd "$X/host/simhid" && mkdir -p SimulatorHID.framework
mac 'rm -rf /tmp/simhid && mkdir -p /tmp/simhid && cat > /tmp/simhid/simhid.m && cd /tmp/simhid && xcrun -sdk iphonesimulator clang -target x86_64-apple-ios17.0-simulator -O1 -Wall -Wno-unused-parameter -fno-objc-arc -dynamiclib -install_name /System/Library/PrivateFrameworks/SimulatorHID.framework/SimulatorHID -framework Foundation -framework IOKit -lobjc -o SimulatorHID simhid.m >&2 && cat SimulatorHID' < simhid.m > SimulatorHID.framework/SimulatorHID.new
mv SimulatorHID.framework/SimulatorHID.new SimulatorHID.framework/SimulatorHID
python3 - <<'PY'
import plistlib
plistlib.dump({"CFBundleExecutable": "SimulatorHID", "CFBundleIdentifier": "com.x86code.SimulatorHID", "CFBundleName": "SimulatorHID",
  "CFBundlePackageType": "FMWK", "CFBundleInfoDictionaryVersion": "6.0", "CFBundleVersion": "1", "NSPrincipalClass": "SimHIDSystem",
  "CFBundleSupportedPlatforms": ["iPhoneSimulator"]}, open("SimulatorHID.framework/Info.plist", "wb"))
PY
ls -la SimulatorHID.framework
