#!/bin/sh
# x86Code: build the small launchd jobs a standard boot runs on the Mac ($X86SIM_MAC), x86_64 iOS simulator:
#   host/netconfig/netconfig   (host network configuration into the host-configd store)
#   host/openurl/openurl       (opens URLs/apps for tests; carries its entitlements in __TEXT,__entitlements)
#   tests/probes/prefs         (cfprefsd write/read check; rundev greps its log)
set -e
. "$(dirname "$0")/mac-env.sh"
CC="$SIMCC -O1 -Wall -Wno-deprecated-declarations"
W=$(mktemp -d "$X86SIM_SCRATCH/macbuild.XXXXXX")
tar cf - -C "$X/host/netconfig" netconfig.m -C "$X/host/openurl" openurl.m openurl.entitlements -C "$X/tests/probes" prefs.c fsevprobe.c fsevhist.c | mac "
rm -rf /tmp/x86jobs && mkdir -p /tmp/x86jobs && cd /tmp/x86jobs && tar xf - &&
$CC -fobjc-arc -framework Foundation -framework SystemConfiguration -o netconfig netconfig.m >&2 &&
$CC -fobjc-arc -framework Foundation -Wl,-sectcreate,__TEXT,__entitlements,openurl.entitlements -o openurl openurl.m >&2 &&
$CC -framework CoreFoundation -o prefs prefs.c >&2 &&
$CC -framework CoreFoundation -o fsevprobe fsevprobe.c >&2 &&
$CC -framework CoreFoundation -o fsevhist fsevhist.c >&2 &&
tar cf - netconfig openurl prefs fsevprobe fsevhist" | tar xf - -C "$W"
mv "$W/netconfig" "$X/host/netconfig/netconfig"; mv "$W/openurl" "$X/host/openurl/openurl"; mv "$W/prefs" "$X/tests/probes/prefs"; mv "$W/fsevprobe" "$X/tests/probes/fsevprobe"; mv "$W/fsevhist" "$X/tests/probes/fsevhist"; rmdir "$W"
ls -la "$X/host/netconfig/netconfig" "$X/host/openurl/openurl" "$X/tests/probes/prefs"
