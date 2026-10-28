#!/bin/sh
# x86Code: build the iOS-simulator test apps tests/apps/<Name> (x86_64 only, no Xcode project) on the Mac ($X86SIM_MAC)
# and copy the ad-hoc signed <Name>.app bundles back to tests/apps/build/. The sources and the icon helper go over ssh
# as a tar, the build runs in /tmp/x86apps on the Mac and that directory is removed afterwards; nothing stays there.
#   build/mac-build-apps.sh [name ...]     (default: HelloUIKit SwiftUIDemo MetalDemo SpriteDemo DevTools)
set -e
. "$(dirname "$0")/mac-env.sh"
APPS=${*:-HelloUIKit SwiftUIDemo MetalDemo SpriteDemo DevTools}
for n in $APPS; do
	[ -f "$X/tests/apps/$n/Info.plist" ] || { echo "no app $n (tests/apps/$n/Info.plist)" >&2; exit 1; }
done
W=$(mktemp -d "${TMPDIR:-/tmp}/x86apps.XXXXXX")
trap 'rm -rf "$W"' EXIT

# the Mac half: compile each app, assemble the bundle, icons, entitlements, ad-hoc signature, verify; the .app tar goes to stdout
cat > "$W/remote.sh" <<'REMOTE'
#!/bin/sh
# x86Code: remote half of build/mac-build-apps.sh (runs in /tmp/x86apps on the Mac; logs go to stderr, the tar to stdout)
set -e
trap 'cd / && rm -rf /tmp/x86apps' EXIT
TGT=x86_64-apple-ios26.0-simulator
xcrun -sdk macosx swiftc -O -o mkicon icon-gen.swift >&2
mkdir -p out
for app in $(cat apps.txt); do
	src=$app
	b=out/$app.app
	mkdir -p "$b"
	xcrun -sdk iphonesimulator swiftc -target $TGT -O -parse-as-library -module-name "$app" \
		-o "$b/$app" "$src"/*.swift >&2
	cp "$src/Info.plist" "$b/Info.plist"
	plutil -lint "$b/Info.plist" >&2
	printf 'APPL????' > "$b/PkgInfo"
	case $app in
		HelloUIKit) c=008080; l=H ;;
		SwiftUIDemo) c=0A84FF; l=S ;;
		MetalDemo) c=6A1B9A; l=M ;;
		SpriteDemo) c=F57C00; l=P ;;
		DevTools) c=2E7D32; l=D ;;
		*) c=444444; l=A ;;
	esac
	./mkicon "$b/AppIcon60x60@2x.png" 120 $l $c
	./mkicon "$b/AppIcon60x60@3x.png" 180 $l $c
	id=$(plutil -extract CFBundleIdentifier raw "$b/Info.plist")
	cat > ent.plist <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>com.apple.security.get-task-allow</key>
	<true/>
	<key>application-identifier</key>
	<string>0000000000.$id</string>
</dict>
</plist>
PLIST
	codesign -s - --force --entitlements ent.plist "$b" >&2
	codesign --verify --strict "$b" >&2
	# verify: x86_64 Mach-O, Info.plist and both icons present
	ft=$(file -b "$b/$app")
	case $ft in *x86_64*) ;; *) echo "$app: not x86_64: $ft" >&2; exit 1 ;; esac
	echo "$app: $ft" >&2
	lipo -info "$b/$app" >&2
	for f in Info.plist AppIcon60x60@2x.png AppIcon60x60@3x.png; do
		[ -f "$b/$f" ] || { echo "$app: missing $f" >&2; exit 1; }
	done
	echo "built $b" >&2
done
tar cf - -C out .
REMOTE

echo "$APPS" | tr ' ' '\n' > "$W/apps.txt"
# the sources: tests/apps/<Name> (Swift + Info.plist) and the icon helper; nothing else is sent
tar cf - -C "$W" remote.sh apps.txt -C "$X/tests/apps" icon-gen.swift $APPS |
	mac "rm -rf /tmp/x86apps && mkdir -p /tmp/x86apps && cd /tmp/x86apps && tar xf - && sh ./remote.sh" > "$W/out.tar"
rm -rf "$X/tests/apps/build"
mkdir -p "$X/tests/apps/build"
tar xf "$W/out.tar" -C "$X/tests/apps/build"
ls -ld "$X"/tests/apps/build/*.app
