#!/bin/sh
# Usage: run-launchd-sim.sh <device dir>   (made by mksimdevice.py)
# Starts the runtime's launchd_sim under Darling with CoreSimulator's environment, via simhost.
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
D=$(realpath "$1")
R="/Volumes/SystemRoot$X86SIM_RUNTIME"
ENVS=$(sed 's/^/"/; s/$/"/' "$D/sim.env" | tr '\n' ' ')
# x86Code: frameworks that a Mac host has in /System/Library/Frameworks and the simulator opens there by absolute path
# (not under its runtime root). CoreSpotlight loads its localized strings with
# [NSBundle bundleWithPath:@"/System/Library/Frameworks/CoreSpotlight.framework"]; with no such bundle every string
# was nil and Spotlight crashed on its first search (-[_CSSuggestionToken init...]: "attempt to insert nil object").
# Link the runtime's copy into the prefix where Darling has none.
F="$X86SIM_PREFIX/libexec/darling/System/Library/Frameworks"
for fw in CoreSpotlight; do
	[ -e "$F/$fw.framework" ] || [ -L "$F/$fw.framework" ] || ln -s "$R/System/Library/Frameworks/$fw.framework" "$F/$fw.framework"
done
# Start from a fresh container: leftovers of earlier runs survive killing simhost,
# two launchd_sims on one device fight over its state, and killing processes inside
# a running container leaves darlingserver in a state where new jobs hang.
"$X/tools/debug/killsim.sh"
"$X86SIM_DARLING" shutdown 2>/dev/null
sleep 2
timeout 30 "$X86SIM_DARLING" shell true < /dev/null > /dev/null 2>&1
# x86Code: the host's mDNSResponder socket (linux/mdnsd, started by cli/libexec/rundev.sh) where the simulator looks for it
[ "${X86CODE_MDNS:-1}" != 0 ] && timeout 30 "$X86SIM_DARLING" shell ln -sf "/Volumes/SystemRoot$X86SIM_STATE/mDNSResponder" /var/run/mDNSResponder < /dev/null > /dev/null 2>&1
# x86Code: the time zone. A simulator on a Mac reads the host's /etc/localtime (simulator processes see the host's
# file system); Darling's prefix has none, so the device ran on UTC (the status bar was 6 h off here). Link it to the
# Linux host's zone (Darling's /usr/share/zoneinfo is Linux's; same TZif files).
TZNAME=$(readlink -f /etc/localtime 2>/dev/null | sed -n 's#^.*/zoneinfo/##p')
[ -n "$TZNAME" ] && [ -f "/usr/share/zoneinfo/$TZNAME" ] && \
	timeout 30 "$X86SIM_DARLING" shell ln -sf "/usr/share/zoneinfo/$TZNAME" /private/etc/localtime < /dev/null > /dev/null 2>&1
# simhost (host/simhost) plays CoreSimulator's part: bootstrap port, service port, holding tank
H="/Volumes/SystemRoot$X/host/simhost/simhost"
exec timeout "${TIMEOUT:-60}" "$X86SIM_DARLING" shell sh -c "exec \"$H\" /usr/bin/env $ENVS $SIMENV \"$R/sbin/launchd_sim\" \"/Volumes/SystemRoot$D/data/var/run/launchd_bootstrap.plist\"" < /dev/null
