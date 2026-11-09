# x86Code: sourced by build/mac-build-*.sh. The simulator-side binaries (host/, tests/) are Mach-O for the arm64 iOS
# simulator (simhost: macOS x86_64, Darling's host side) and are compiled on a Mac with Xcode over ssh ($X86SIM_MAC, x86sim.env):
# the sources go there by stdin/tar, the binaries come back the same way. Nothing else is kept on the Mac.
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
# X86SIM_BUILD=linux (default when sdks/iPhoneSimulator.sdk exists): the same commands run here with Linux clang/lld and
# the SDKs in sdks/ (build/linux-env.sh); X86SIM_BUILD=mac: over ssh on the Mac.
[ -n "${X86SIM_BUILD:-}" ] || { [ -d "$X/sdks/iPhoneSimulator.sdk" ] && X86SIM_BUILD=linux || X86SIM_BUILD=mac; }
export X86SIM_BUILD
if [ "$X86SIM_BUILD" = linux ]; then . "$X/build/linux-env.sh"; else
MAC=${X86SIM_MAC:?set X86SIM_MAC (ssh host of a Mac with Xcode, e.g. user@mac.local) in x86sim.env}
SIMCC="xcrun -sdk iphonesimulator clang -target arm64-apple-ios17.0-simulator"
mac() { ssh -o BatchMode=yes "$MAC" "$@"; }
fi
