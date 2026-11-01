#!/bin/sh
# x86Code (Speed 27): host files an iOS simulator reads from the Mac's own /System/Library by absolute path (not from
# its runtime root), fetched once from the Mac ($X86SIM_MAC, mac-env.sh) into runtimes/macos-host (gitignored: Apple
# data, never committed). boot-a64.sh links them into the Darling prefix.
#   GeoServicesData: geod's seed resource manifest (ResourceManifest.json + region/territory/time zone archives).
#   Without it geod had no active tile group and Maps drew a bare globe grid, requesting no tiles.
# Usage: cli/libexec/arm64/fetch-macos-host.sh
set -e
X=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}
. "$X/build/mac-env.sh"
H=$X/runtimes/macos-host
mkdir -p "$H/System/Library"
mac "sw_vers -productVersion" > "$H/macos-version.txt"
for d in GeoServicesData; do
	mac "tar cf - -C /System/Library $d" | tar xf - -C "$H/System/Library" 2>/dev/null
	echo "fetched /System/Library/$d (macOS $(cat "$H/macos-version.txt"))"
done
