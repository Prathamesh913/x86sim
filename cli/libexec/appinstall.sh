#!/bin/bash
# x86Code: `x86sim install|uninstall|launch`, as `xcrun simctl install|uninstall|launch` on a Mac.
#   install <path.app>             copy the bundle into the device and have installd install it (through
#                                  LaunchServices -> InstallCoordination -> installd, in host/openurl inside the device)
#   uninstall <bundle id>
#   launch <bundle id> [args...]   open through FrontBoard (as simctl launch), arguments passed to the app
# Each waits for the helper's answer in <device>/data/var/log/x86code-openurl.log and prints it.
. "$(dirname "$0")/../lib/config.sh"
D=$X86SIM_DEVICE/data
LOG=$D/var/log/x86code-openurl.log
Q=$D/tmp/x86code-openurl
cmd=$1; shift
request() {  # request LINE PATTERN TIMEOUT
	local n; n=$(wc -l < "$LOG" 2>/dev/null || echo 0)
	printf '%s\n' "$1" > "$Q.new" && mv "$Q.new" "$Q"
	for _ in $(seq $(( $3 * 2 ))); do
		sleep 0.5
		r=$(tail -n +$((n + 1)) "$LOG" 2>/dev/null | grep -m1 -E "$2") && { echo "$r"; [[ $r != *failed* && $r != *error* ]]; return; }
	done
	echo "$cmd: no answer in $3 s (is the device booted? see $LOG)" >&2; return 1
}
case $cmd in
install)
	app=${1%/}; [ -f "$app/Info.plist" ] || { echo "usage: x86sim install <path.app>" >&2; exit 2; }
	# staging copy inside the device's data directory (visible inside Darling); installd copies it again
	S=$D/tmp/x86code-install; mkdir -p "$S"; rm -rf "$S/$(basename "$app")"
	cp -a "$app" "$S/" || exit 1
	request "install tmp/x86code-install/$(basename "$app")" "^install .*$(basename "$app")" 120 ;;
uninstall)
	[ -n "$1" ] || { echo "usage: x86sim uninstall <bundle id>" >&2; exit 2; }
	request "uninstall $1" "^uninstall $1:" 60 ;;
launch)
	[ -n "$1" ] || { echo "usage: x86sim launch <bundle id> [args...]" >&2; exit 2; }
	line="app $(IFS=$'\t'; echo "$*")"
	request "$line" "^openurl $1 -> " 40 ;;
*) echo "appinstall.sh install|uninstall|launch" >&2; exit 2 ;;
esac
