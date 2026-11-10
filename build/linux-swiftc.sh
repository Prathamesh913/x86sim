#!/bin/bash
# x86Sim: drop-in for `xcrun -sdk <sdk> swiftc ...` on Linux. Compiles Swift for the iOS simulator / macOS (Mach-O)
# with the swift.org Linux toolchain against the Apple SDKs in sdks/, links with ld64.lld (ad-hoc signed for arm64).
#   build/linux-swiftc.sh [-sdk iphonesimulator|macosx|<path>] [-target <triple>] <swiftc args...>
# Without -sdk the SDK follows -target (*-simulator -> iPhoneSimulator.sdk, *-macos* -> MacOSX.sdk); without -target
# it is arm64 for that SDK (macOS: arm64-apple-macos<SDK major>.0, like a Mac host build).
# Needs (all gitignored under sdks/):
#   sdks/swift-6.4.2/       swift.org swift-6.4.2-RELEASE-ubi10 tarball, extracted (+ compat-lib/libncurses.so.6 shim)
#   sdks/swift-lib/         Darwin resource dir copied from Xcode's XcodeDefault.xctoolchain/usr/lib/swift:
#                           {iphonesimulator,macosx}/libswiftCompatibility*.a etc., apinotes/,
#                           clang/lib/darwin/libclang_rt.{iossim,osx}.a; clang/include -> toolchain clang headers
# Env: SWIFT_TOOLCHAIN (default sdks/swift-6.4.2), SWIFT_RESOURCE_DIR (default sdks/swift-lib),
#      LD64_LLD (default ld64.lld on PATH), X86SIM_SDKS (default <repo>/sdks).
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
SDKS=${X86SIM_SDKS:-$R/sdks}
TC=${SWIFT_TOOLCHAIN:-$SDKS/swift-6.4.2}
RES=${SWIFT_RESOURCE_DIR:-$SDKS/swift-lib}
LLD=${LD64_LLD:-$(command -v ld64.lld || true)}
die() { echo "linux-swiftc: $*" >&2; exit 2; }
[ -x "$TC/usr/bin/swiftc" ] || die "no toolchain at $TC (swift-6.4.2-RELEASE-ubi10)"
[ -d "$RES/iphonesimulator" ] && [ -d "$RES/clang/lib/darwin" ] || die "no Darwin resource dir at $RES"
[ -n "$LLD" ] && [ -x "$LLD" ] || die "ld64.lld not found (install lld)"

# pull -sdk / -target out of the arguments (both "-x v" and "-x=v" forms)
sdk= tgt= args=()
while [ $# -gt 0 ]; do
	case $1 in
		-sdk) sdk=${2:?-sdk needs a value}; shift 2 ;;
		-sdk=*) sdk=${1#-sdk=}; shift ;;
		-target) tgt=${2:?-target needs a value}; shift 2 ;;
		-target=*) tgt=${1#-target=}; shift ;;
		*) args+=("$1"); shift ;;
	esac
done
case $sdk in
	iphonesimulator*) sdk=$SDKS/iPhoneSimulator.sdk ;;
	macosx*) sdk=$SDKS/MacOSX.sdk ;;
	'') case $tgt in
		*-simulator) sdk=$SDKS/iPhoneSimulator.sdk ;;
		*-macos*|'') sdk=$SDKS/MacOSX.sdk ;;
		*) die "no SDK for target $tgt" ;;
	esac ;;
esac
[ -f "$sdk/SDKSettings.json" ] || die "not an SDK: $sdk"
if [ -z "$tgt" ]; then
	case $(basename "$sdk") in
		iPhoneSimulator*) tgt=arm64-apple-ios17.0-simulator ;;
		*) v=$(sed -n 's/.*"Version" *: *"\([0-9]*\).*/\1/p' "$sdk/SDKSettings.json" | head -1); tgt=arm64-apple-macos${v:-14}.0 ;;
	esac
fi

# the ubi10 build links libncurses.so.6; Arch only ships libncursesw.so.6 (compat-lib holds the symlink)
[ -d "$TC/compat-lib" ] && export LD_LIBRARY_PATH=$TC/compat-lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
# -resource-dir: a Darwin-layout dir, because the Linux one (usr/lib/swift) carries corelibs os/ and dispatch/ headers
#   that shadow the SDK's; it also supplies the compat libs (-L <res>/<platform>) and libclang_rt.<plat>.a to clang.
# -Isystem <sdk>/usr/lib/swift/shims: Apple's frontend adds this itself (SwiftShims for the SDK's Swift.swiftinterface).
# Linking: the driver runs the toolchain clang as linker driver, which invokes ld64.lld (ad-hoc linker signature);
#   -isysroot makes that clang read SDKSettings.json (LC_BUILD_VERSION sdk 27.2 rather than sdk = minos).
# Macros: the toolchain's own plugins (Swift/Observation/Foundation macros) work; SwiftUI's @State needs SwiftUIMacros,
#   whose Apple build is a macOS-only binary, so build/swiftui-macros/ is compiled for Linux (once, kept up to date).
PLUG=$RES/host-plugins; MSRC=$R/build/swiftui-macros/SwiftUIMacros.swift
if [ ! -f "$PLUG/libSwiftUIMacros.so" ] || [ "$MSRC" -nt "$PLUG/libSwiftUIMacros.so" ]; then
	H=$TC/usr/lib/swift/host; mkdir -p "$PLUG"
	"$TC/usr/bin/swiftc" -O -emit-library -module-name SwiftUIMacros -I "$H" -L "$H" -lSwiftSyntax -lSwiftSyntaxMacros \
		-lSwiftSyntaxBuilder -lSwiftDiagnostics -Xlinker -rpath -Xlinker "$H" -module-cache-path "$PLUG/.mc" \
		"$MSRC" -o "$PLUG/libSwiftUIMacros.so.$$" >&2 && mv "$PLUG/libSwiftUIMacros.so.$$" "$PLUG/libSwiftUIMacros.so" ||
		die "building the SwiftUIMacros plugin failed"
fi
exec "$TC/usr/bin/swiftc" -sdk "$sdk" -target "$tgt" -resource-dir "$RES" -Isystem "$sdk/usr/lib/swift/shims" \
	-plugin-path "$PLUG" -use-ld=lld -ld-path="$LLD" -Xclang-linker -resource-dir -Xclang-linker "$RES/clang" \
	-Xclang-linker -isysroot -Xclang-linker "$sdk" "${args[@]}"
