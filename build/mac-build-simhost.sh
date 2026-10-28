#!/bin/sh
# Build host/simhost/simhost (macOS x86_64, runs inside Darling) on the Mac ($X86SIM_MAC)
. "$(dirname "$0")/mac-env.sh"
cd "$X/host/simhost"
mac 'mkdir -p /tmp/simhost && cat > /tmp/simhost/simhost.c && cd /tmp/simhost && xcrun clang -arch x86_64 -mmacosx-version-min=10.15 -O1 -Wall -o simhost simhost.c >&2 && cat simhost' < simhost.c > simhost.new && mv simhost.new simhost && chmod +x simhost
