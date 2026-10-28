#!/bin/sh
# Build host/fpsim2host/fpsim2host (x86_64 iOS simulator) on the Mac ($X86SIM_MAC)
. "$(dirname "$0")/mac-env.sh"
cd "$X/host/fpsim2host"
mac 'mkdir -p /tmp/fps2h && cat > /tmp/fps2h/fpsim2host.m && cd /tmp/fps2h && xcrun -sdk iphonesimulator clang -target x86_64-apple-ios17.0-simulator -O1 -Wall -Wno-unused-variable -fobjc-arc -framework Foundation -o fpsim2host fpsim2host.m >&2 && cat fpsim2host' < fpsim2host.m > fpsim2host.new && chmod +x fpsim2host.new && mv fpsim2host.new fpsim2host
