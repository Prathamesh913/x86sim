#!/bin/sh
# Kill every Darling process (launchers and all mldr processes, i.e. all containers):
# leftovers of earlier runs survive `darling shutdown` and confuse new runs. Matches on the executable, so
# shells whose command line mentions these paths are not hit. Needs root for the
# setuid launchers: SUDO_PASS or a cached sudo.
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
pids=""
for d in /proc/[0-9]*; do
	p=${d#/proc/}
	[ "$p" = "$$" ] && continue
	exe=$(readlink "$d/exe" 2>/dev/null) || continue
	case "$exe" in
		"$X86SIM_DARLING") pids="$pids $p" ;;
		"$X86SIM_PREFIX/libexec/darling/usr/libexec/darling/mldr") pids="$pids $p" ;;
	esac
done
[ -z "$pids" ] && exit 0
x86sim_sudo kill -9 $pids 2>/dev/null || kill -9 $pids 2>/dev/null
exit 0
