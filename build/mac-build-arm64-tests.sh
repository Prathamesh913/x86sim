#!/bin/bash
# Stage 0 (arm64 plan): build tests/arm64 on the Mac as plain arm64 iOS-simulator binaries, plus macOS arm64 baselines.
# Usage on the Mac: build/mac-build-arm64-tests.sh <srcdir> <outdir>
set -euo pipefail
S=${1:?src}; O=${2:?out}; mkdir -p "$O"
SIM="xcrun -sdk iphonesimulator clang -arch arm64 -mios-simulator-version-min=17.0 -O2"
MAC="xcrun -sdk macosx clang -arch arm64 -O2"
for t in hello bench threads signals fp16 fp16v sharestress icstress; do $SIM -o "$O/$t.sim" "$S/$t.c"; done
$MAC -o "$O/bench.mac" "$S/bench.c"
xcrun -sdk iphonesimulator clang -arch arm64 -mios-simulator-version-min=17.0 -O1 -fobjc-arc -framework Foundation -o "$O/cfprobe.sim" "$S/cfprobe.m"
xcrun -sdk iphonesimulator clang -arch arm64 -mios-simulator-version-min=17.0 -O1 -fobjc-arc -framework Foundation -o "$O/threxit.sim" "$S/threxit.m"
xcrun -sdk iphonesimulator clang -arch arm64 -mios-simulator-version-min=17.0 -O1 -fobjc-arc -framework Foundation -o "$O/plist.sim" "$S/plist.m"
for k in sim mac; do
  if [ $k = sim ]; then sdk=iphonesimulator; tgt=arm64-apple-ios17.0-simulator; else sdk=macosx; tgt=arm64-apple-macos14.0; fi
  xcrun -sdk $sdk clang -target $tgt -O2 -fno-objc-arc -c "$S/objcpart.m" -o "$O/objcpart.$k.o"
  xcrun -sdk $sdk swiftc -target $tgt -O "$S/objcswift.swift" "$O/objcpart.$k.o" -lobjc -o "$O/objcswift.$k"
done
rm -f "$O"/*.o
ls -l "$O"
