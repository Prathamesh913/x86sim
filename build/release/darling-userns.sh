#!/bin/sh
# x86Code: Darling without root (the AppImage's "userns" mode, docs/RELEASE.md). Installed as bin/darling of
# <data>/app/<version>/darling-userns, which <data>/darling-prefix points at.
# Darling's binaries carry their install prefix (/opt/x86code/darling) and bin/darling wants to be root to set up a
# container (namespaces, overlay mount). Here a long-lived "holder" process owns an unprivileged user namespace (the
# user is root inside it, nobody else outside) with its own mount namespace, where the AppImage's Darling tree is
# bind-mounted at /opt/x86code/darling; every darling command joins it with nsenter. Containers live in that namespace,
# so the holder is kept per Darling build (pidfile in the cache dir) and restarted if gone.
# Inside the namespace /opt shows only x86code/ (the rest of the host's /opt is hidden from Darling, not from Linux).
set -e
W=$(cd "$(dirname "$0")/.." && pwd -P)
TREE=$(cd "$W/../darling" && pwd -P)
ID=$(cat "$W/.x86code-complete")
C=${X86SIM_SCRATCH:-${XDG_CACHE_HOME:-$HOME/.cache}/x86code}
PIDF=$C/userns-$ID.pid
O=$C/userns-opt-$ID
holder() {
	p=$(cat "$PIDF" 2>/dev/null) || return 1
	[ -n "$p" ] && [ -e "/proc/$p/root/opt/x86code/.holder-$ID" ] && echo "$p"
}
if ! p=$(holder); then
	mkdir -p "$O/x86code"
	ln -sfn "$TREE" "$O/x86code/darling"
	: > "$O/x86code/.holder-$ID"
	rm -f "$PIDF"
	setsid unshare -r -m sh -c 'mount --bind "$1" /opt && echo $$ > "$2" && exec sleep 2147483647' sh "$O" "$PIDF" \
		< /dev/null > "$C/userns-$ID.log" 2>&1 &
	i=0
	until p=$(holder); do
		i=$((i + 1))
		[ $i -lt 50 ] || { echo "darling (userns): no namespace holder, see $C/userns-$ID.log" >&2; exit 1; }
		sleep 0.1
	done
fi
exec nsenter --preserve-credentials -t "$p" -U -m -- /opt/x86code/darling/bin/darling "$@"
