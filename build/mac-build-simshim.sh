#!/bin/sh
# Build host/simshim/compat/libsimshim.dylib (x86_64 iOS simulator) on the Mac ($X86SIM_MAC)
. "$(dirname "$0")/mac-env.sh"
cd "$X/host/simshim/compat"
mac 'mkdir -p /tmp/sh && cat > /tmp/sh/simshim.c && cd /tmp/sh && xcrun -sdk iphonesimulator clang -target x86_64-apple-ios17.0-simulator -O1 -Wall -dynamiclib -framework IOKit -framework CoreFoundation -lobjc -lsqlite3 -o libsimshim.dylib simshim.c >&2 && cat libsimshim.dylib' < simshim.c > libsimshim.dylib.new && mv libsimshim.dylib.new libsimshim.dylib
