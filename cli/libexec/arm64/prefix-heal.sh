#!/bin/sh
# x86Code: make the Darling pieces a boot uses match our build, so a recreated prefix can't lose fixes again.
# (2026-11-01: $X86SIM_SCRATCH/a64prefix was recreated and lost the libsystem_kernel installed there in Speed 12, whose
# dserver RPC fix stops "Bad S2C call: no FD, but expected one" aborts; backboardd then died on Photos/Maps launches.)
#
# Each manifest line: <path under external/darling/build> <path under the Darling install root>. A piece whose build
# output differs from the installed one is copied into the install root (darling-prefix/libexec/darling, the lower
# layer every prefix overlays; copy + rename, so running processes keep their mapping). A copy in the prefix's own
# upper layer that differs would shadow it: it is replaced the same way (only when the prefix is already set up:
# Darling treats an existing prefix dir as initialized, so a fresh one is never created here).
# A third field "platform": the piece gets tools/debug/set_macho_platform.py (as tools/debug/dinstall.sh does) before
# it is compared and installed.
# Speed 27: libsystem_c (Darling's libkqueue is linked into it) and libkqueue_sim (the simulator's kqueue) joined: the
# close() knote fix (libkqueue kqueue_closed_fd) lives in both.
# Usage: prefix-heal.sh <prefix dir>        (A64_PREFIX_HEAL=0 in boot-a64.sh: skip)
X=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}
P=$1
B=$X/external/darling/build
I=${X86SIM_DARLING_ROOT:-$X/darling-prefix/libexec/darling}
MANIFEST="
src/external/xnu/darling/src/libsystem_kernel/libsystem_kernel.dylib usr/lib/system/libsystem_kernel.dylib
src/external/libc/libsystem_c.dylib usr/lib/system/libsystem_c.dylib
src/external/libkqueue/libkqueue_sim.dylib usr/lib/darling/libkqueue_sim.dylib platform
"
install1() {  # <src> <dst>
	mkdir -p "$(dirname "$2")" && cp "$1" "$2.heal.$$" && mv -f "$2.heal.$$" "$2" && echo "  prefix-heal: installed $2"
}
TMPD=$(mktemp -d /tmp/x86code-heal.XXXXXX) || exit 0
echo "$MANIFEST" | while read -r src dst opt; do
	[ -n "$src" ] || continue
	if [ ! -f "$B/$src" ]; then echo "  prefix-heal: no build output $src (not built?)"; continue; fi
	f=$B/$src
	if [ "$opt" = platform ]; then
		f=$TMPD/$(basename "$src"); cp "$B/$src" "$f" && python3 -I "$X/tools/debug/set_macho_platform.py" "$f" >/dev/null || continue
	fi
	cmp -s "$f" "$I/$dst" || install1 "$f" "$I/$dst"
	if [ -n "$P" ] && [ -f "$P/$dst" ] && ! cmp -s "$f" "$P/$dst"; then install1 "$f" "$P/$dst"; fi
done
rm -rf "$TMPD"
exit 0
