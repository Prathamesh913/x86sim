#!/bin/sh
# Build host/simfb/SimFramebuffer.framework (x86_64 iOS simulator) on the Mac ($X86SIM_MAC)
. "$(dirname "$0")/mac-env.sh"
cd "$X/host/simfb" && mkdir -p SimFramebuffer.framework
mac 'mkdir -p /tmp/simfb && cat > /tmp/simfb/simfb.c && cd /tmp/simfb && xcrun -sdk iphonesimulator clang -target x86_64-apple-ios17.0-simulator -O1 -Wall -Wno-unused-parameter -fvisibility=hidden -dynamiclib -install_name /System/Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer -framework CoreFoundation -framework CoreGraphics -framework IOSurface -lobjc -o SimFramebuffer simfb.c >&2 && cat SimFramebuffer' < simfb.c > SimFramebuffer.framework/SimFramebuffer.new && mv SimFramebuffer.framework/SimFramebuffer.new SimFramebuffer.framework/SimFramebuffer
