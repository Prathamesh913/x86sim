#!/bin/sh
# Build host/simshim/compat/libsimshim.dylib (x86_64 iOS simulator) on the Mac ($X86SIM_MAC)
# x86Code: with host/simshim/diag/oslogdiag.m built in (-DSIMSHIM_OSLOG): os_log to a file for a process started with
# X86CODE_OSLOG + X86CODE_OSLOG_DIR in its environment (x86sim launch --oslog, x86sim log stream --app); off otherwise.
# No Foundation link: the ObjC parts only use libobjc (objc_msgSend), so processes without Foundation stay without it.
. "$(dirname "$0")/mac-env.sh"
W=$(mktemp -d "$X86SIM_SCRATCH/macbuild.XXXXXX")
tar cf - -C "$X/host/simshim/compat" simshim.c -C "$X/host/simshim/diag" oslogdiag.m diagpath.h | mac '
rm -rf /tmp/sh && mkdir -p /tmp/sh && cd /tmp/sh && tar xf - &&
cc() { xcrun -sdk iphonesimulator clang -target x86_64-apple-ios17.0-simulator -O1 -Wall -Wno-unused-variable -Wno-deprecated-declarations "$@"; } &&
cc -c -o simshim.o simshim.c >&2 &&
cc -c -DSIMSHIM_OSLOG -o oslog.o oslogdiag.m >&2 &&
cc -dynamiclib -framework IOKit -framework CoreFoundation -lobjc -lsqlite3 -o libsimshim.dylib simshim.o oslog.o >&2 &&
cat libsimshim.dylib; cd / && rm -rf /tmp/sh' > "$W/libsimshim.dylib" && [ -s "$W/libsimshim.dylib" ] &&
mv "$W/libsimshim.dylib" "$X/host/simshim/compat/libsimshim.dylib"; rm -rf "$W"
ls -la "$X/host/simshim/compat/libsimshim.dylib"
