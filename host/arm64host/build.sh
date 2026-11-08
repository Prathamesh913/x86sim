#!/bin/sh
# Builds arm64host (x86_64 macOS Mach-O) on Linux with Darling's in-tree toolchain (clang + ld64 from cctools-port).
# Usage: host/arm64host/build.sh [darling build dir]   (ARM64HOST_OUT: output path, default host/arm64host/arm64host)
set -e
H=$(cd "$(dirname "$0")" && pwd)
D=$(cd "$H/../../external/darling" && pwd)
B=${1:-$D/build}
SDK=$D/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
clang -target x86_64-apple-darwin20 -mmacosx-version-min=11.0 -isysroot "$SDK" -O2 -Wall \
	-fuse-ld="$B/src/external/cctools-port/cctools/ld64/src/x86_64-apple-darwin20-ld" \
	-B "$B/src/external/cctools-port/cctools/ld64/src/" \
	-Wl,-dylib_file,/usr/lib/libSystem.B.dylib:"$B/src/external/libsystem/libSystem.B.dylib" -L"$B/src/external/libsystem" \
	-o "${ARM64HOST_OUT:-$H/arm64host}" "$H/arm64host.c"
