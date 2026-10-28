# x86Code: sourced by build/mac-build-*.sh. The simulator-side binaries (host/, tests/) are Mach-O for the x86_64 iOS
# simulator (or macOS x86_64 for simhost) and are compiled on a Mac with Xcode over ssh ($X86SIM_MAC, x86sim.env):
# the sources go there by stdin/tar, the binaries come back the same way. Nothing else is kept on the Mac.
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
MAC=${X86SIM_MAC:?set X86SIM_MAC (ssh host of a Mac with Xcode, e.g. user@mac.local) in x86sim.env}
SIMCC="xcrun -sdk iphonesimulator clang -target x86_64-apple-ios17.0-simulator"
mac() { ssh -o BatchMode=yes "$MAC" "$@"; }
