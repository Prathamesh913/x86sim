#!/bin/sh
# x86Sim: the one privileged step of the AppImage setup, run as root by pkexec (or sudo):
#   install-helper.sh install <staged Darling dir> <darling id>
#   install-helper.sh uninstall
# Darling's launcher bin/darling must be setuid root (it creates the container's namespaces and overlay mount, then
# drops to the user), and its install prefix is compiled in (/opt/x86sim/darling: darling execs bin/darlingserver
# from it, darlingserver overlays libexec/darling). An AppImage can carry neither a setuid file nor a fixed path, so the
# Darling tree is copied to /opt/x86sim/darling-<id>, owned by root, and the /opt/x86sim/darling symlink is switched
# to it (atomically: running phones keep the tree they started with). The current and the previous tree are kept.
set -eu
# X86SIM_OPT: tests only (pkexec and sudo clear the environment, so a normal run always uses /opt/x86sim)
T=${X86SIM_OPT:-/opt/x86sim}
[ "$(id -u)" = 0 ] || { echo "install-helper: must run as root" >&2; exit 1; }
case "${1:-}" in
install)
	SRC=$2 ID=$3
	case "$ID" in '' | *[!0-9a-f]*) echo "install-helper: bad id" >&2; exit 2 ;; esac
	[ -f "$SRC/bin/darling" ] && [ -f "$SRC/bin/darlingserver" ] || { echo "install-helper: no Darling in $SRC" >&2; exit 2; }
	D=$T/darling-$ID
	mkdir -p "$T"
	chmod 755 "$T"
	if [ ! -f "$D/.x86sim-complete" ]; then
		rm -rf "$D.tmp"
		cp -a "$SRC" "$D.tmp"
		chown -R root:root "$D.tmp"
		chmod -R go-w "$D.tmp"
		chmod 1777 "$D.tmp/libexec/darling/private/var/tmp" 2>/dev/null || true
		chmod 4755 "$D.tmp/bin/darling"
		echo "$ID" > "$D.tmp/.x86sim-complete"
		rm -rf "$D"
		mv "$D.tmp" "$D"
	fi
	prev=$(readlink "$T/darling" 2>/dev/null || true)
	ln -sfn "darling-$ID" "$T/darling.new"
	mv -Tf "$T/darling.new" "$T/darling"
	# keep the new and the previous tree only
	for d in "$T"/darling-*; do
		b=$(basename "$d")
		[ "$b" = "darling-$ID" ] || [ "$b" = "$prev" ] || rm -rf "$d"
	done
	echo "installed $D"
	# releases up to 0.1.0-pre8 (x86Code) installed into /opt/x86code
	[ -n "${X86SIM_OPT:-}" ] || rm -rf /opt/x86code
	;;
uninstall)
	rm -rf "$T"
	[ -n "${X86SIM_OPT:-}" ] || rm -rf /opt/x86code
	echo "removed $T"
	;;
*)
	echo "usage: install-helper.sh install <dir> <id> | uninstall" >&2
	exit 2
	;;
esac
