# x86Code: sourced by build/mac-env.sh when the simulator-side binaries are built on Linux (X86SIM_BUILD=linux, the
# default when sdks/iPhoneSimulator.sdk exists). Same commands as on the Mac: mac() runs them locally with
# build/linux-bin first in PATH, where xcrun/codesign/lipo/plutil map to Linux clang + ld64.lld and the SDKs in sdks/.
#   SDK_IOS / SDK_MAC   the Apple SDKs (copied from Xcode, never committed)
#   SIMCC               x86_64 iOS simulator (min 17.0); SIMCC_ARM64 the arm64 one; MACCC macOS x86_64 (min 10.15)
X=${X:-$X86SIM_ROOT}
SDK_IOS=${SDK_IOS:-$X/sdks/iPhoneSimulator.sdk}
SDK_MAC=${SDK_MAC:-$X/sdks/MacOSX.sdk}
[ -d "$SDK_IOS" ] && [ -d "$SDK_MAC" ] || { echo "linux build: missing $SDK_IOS or $SDK_MAC (or set X86SIM_BUILD=mac)" >&2; exit 1; }
command -v ld64.lld >/dev/null || { echo "linux build: ld64.lld not found (lld package)" >&2; exit 1; }
export SDK_IOS SDK_MAC X86SIM_ROOT
PATH="$X/build/linux-bin:$PATH"; export PATH
# Apple clang 21 keeps incompatible-pointer-types a warning; upstream clang made it an error
LCC="clang -fuse-ld=lld -Wno-unused-command-line-argument -Wno-error=incompatible-pointer-types"
SIMCC="$LCC -isysroot $SDK_IOS -target x86_64-apple-ios17.0-simulator"
# arm64: no pointer authentication (A64_NOPAUTH, also in linux-bin/xcrun). Upstream clang lowers
# __builtin_return_address to XPACI when the CPU (apple-m1) has pauth; Apple clang emits nothing for non-arm64e, and
# dynarmic does not implement XPACI (runningboardd aborted in libsimshim). Without pauth it is XPACLRI, a HINT (no-op).
A64_NOPAUTH="-Xclang -target-feature -Xclang -pauth"
SIMCC_ARM64="$LCC -isysroot $SDK_IOS -target arm64-apple-ios17.0-simulator $A64_NOPAUTH"
MACCC="$LCC -isysroot $SDK_MAC -target x86_64-apple-macos10.15"
# the "remote" side, run here: a fresh sh in /tmp, sources/output on stdin/stdout as with ssh
mac() { (cd "${TMPDIR:-/tmp}" && sh -c "$*"); }
