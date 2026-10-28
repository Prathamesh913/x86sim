#!/bin/sh
# Build host/fseventsd/fseventsd (x86_64 iOS simulator) on the Mac ($X86SIM_MAC)
. "$(dirname "$0")/mac-env.sh"
cd "$X/host/fseventsd"
mac 'mkdir -p /tmp/fsevd && cat > /tmp/fsevd/fseventsd.c && cd /tmp/fsevd && xcrun -sdk iphonesimulator clang -target x86_64-apple-ios17.0-simulator -O1 -Wall -Wno-unused-variable -Wno-deprecated-declarations -o fseventsd fseventsd.c >&2 && cat fseventsd' < fseventsd.c > fseventsd.new && chmod +x fseventsd.new && mv fseventsd.new fseventsd
