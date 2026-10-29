#!/bin/sh
# Build host/simaudio/libsimaudio.dylib (x86_64 iOS simulator) on the Mac ($X86SIM_MAC).
# x86Code: flat namespace + undefined dynamic_lookup, the CoreAudio references weak: the library interposes the HAL
# API where CoreAudio is loaded and stays inert (and does not load CoreAudio) everywhere else.
. "$(dirname "$0")/mac-env.sh"
W=$(mktemp -d "$X86SIM_SCRATCH/macbuild.XXXXXX")
tar cf - -C "$X/host/simaudio" simaudio.c | mac '
rm -rf /tmp/sa && mkdir -p /tmp/sa && cd /tmp/sa && tar xf - &&
xcrun -sdk iphonesimulator clang -target x86_64-apple-ios17.0-simulator -O2 -Wall -Wno-unused-function -dynamiclib \
  -framework CoreFoundation -Wl,-flat_namespace -Wl,-undefined,dynamic_lookup -o libsimaudio.dylib simaudio.c >&2 &&
cat libsimaudio.dylib; cd / && rm -rf /tmp/sa' > "$W/libsimaudio.dylib" && [ -s "$W/libsimaudio.dylib" ] &&
mv "$W/libsimaudio.dylib" "$X/host/simaudio/libsimaudio.dylib"; rm -rf "$W"
ls -la "$X/host/simaudio/libsimaudio.dylib"
