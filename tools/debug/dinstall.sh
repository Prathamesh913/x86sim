#!/bin/sh
# Rebuild Darling and install to darling-prefix, keeping bin/darling setuid root.
# Needs sudo; reads the password from $SUDO_PASS if set.
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
set -e
D=$X86SIM_DARLING
s() { x86sim_sudo "$@"; }
cd "$X86SIM_DARLING_BUILD" && ninja > $X86SIM_SCRATCH/dbuild.log 2>&1 || { grep -E 'FAILED|error:' $X86SIM_SCRATCH/dbuild.log | head; exit 1; }
$D shutdown 2>/dev/null || true
s chown "$(id -u):$(id -g)" $D
ninja install > $X86SIM_SCRATCH/dinst.log 2>&1 || echo "install failed, see $X86SIM_SCRATCH/dinst.log"
# Helpers injected into iOS simulator processes must carry the iOS-simulator platform
# macOS has /private/var/tmp; launchd_sim keeps its state there
mkdir -p $X86SIM_PREFIX/libexec/darling/private/var/tmp && chmod 1777 $X86SIM_PREFIX/libexec/darling/private/var/tmp
python3 -I $X/tools/debug/set_macho_platform.py $X86SIM_PREFIX/libexec/darling/usr/lib/darling/libkqueue_sim.dylib >/dev/null
s chown root:root $D; s chmod u+s $D
# The first command after a container (re)start can hang; start it now
timeout 30 $D shell true < /dev/null > /dev/null 2>&1 || true
echo installed
