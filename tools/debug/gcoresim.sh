#!/bin/sh
# Dump a core of the Darling process whose command line contains $1 to $X86SIM_SCRATCH/<name>.core (needs SUDO_PASS)
# x86Sim: cores and scratch files go to $X86SIM_SCRATCH (/tmp is a small tmpfs)
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
mkdir -p $X86SIM_SCRATCH
for d in /proc/[0-9]*; do
	case "$(readlink $d/exe 2>/dev/null)" in *mldr) ;; *) continue ;; esac
	tr '\0' ' ' < $d/cmdline 2>/dev/null | grep -q -- "$1" && p=${d#/proc/}
done
[ -n "$p" ] || { echo "no process matching $1" >&2; exit 1; }
n=$(basename "$1")
x86sim_sudo rm -f $X86SIM_SCRATCH/$n.core 2>/dev/null
x86sim_sudo gcore -o $X86SIM_SCRATCH/$n $p >/dev/null 2>&1 && x86sim_sudo mv $X86SIM_SCRATCH/$n.$p $X86SIM_SCRATCH/$n.core && x86sim_sudo chmod 644 $X86SIM_SCRATCH/$n.core && echo $X86SIM_SCRATCH/$n.core
