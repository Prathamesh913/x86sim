#!/bin/sh
# x86Code: build the x86_64 dyld shared cache from this machine's runtime, on Linux, with the runtime's own
# update_dyld_sim_shared_cache (a macOS x86_64 tool: Foundation, CoreFoundation, libobjc, libc++, libsqlite3, all in
# Darling's prefix). It runs in its own short-lived Darling container (a separate prefix, so a booted simulator's
# darlingserver is not touched), inside cli/libexec/limited.sh (40 MB/s disk cap). ~1 min CPU, ~4.4 GB written.
# Then the image table gets this host's inodes (dyldcache.py localize). See docs/notebook/FINDINGS.md (dyld shared cache).
#   dyldcache-build.sh [out dir] [reference dir to compare with]
#   X86SIM_CACHE_PREFIX: the build's Darling prefix (default $X86SIM_SCRATCH/cache-prefix, removed afterwards)
set -e
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
OUT=$(realpath -m "${1:-$X/runtimes/dyld-cache-23F73-x86_64}")
REF=$2
RES=$(dirname "$X86SIM_RUNTIME")               # .../Contents/Resources
P=${X86SIM_CACHE_PREFIX:-${X86SIM_SCRATCH:-$HOME/scratch/x86code}/cache-prefix}
[ -f "$OUT/dyld_sim_shared_cache_x86_64" ] && { echo "$OUT already has a cache (remove it to rebuild)" >&2; exit 1; }
[ -x "$RES/update_dyld_sim_shared_cache" ] || { echo "no update_dyld_sim_shared_cache in $RES" >&2; exit 1; }
mkdir -p "$OUT" "$P"
V=/Volumes/SystemRoot
trap 'DPREFIX="$P" "$X86SIM_DARLING" shutdown >/dev/null 2>&1 || true' EXIT
DPREFIX="$P" "$X/cli/libexec/limited.sh" "$X86SIM_DARLING" shell "$V$RES/update_dyld_sim_shared_cache" \
	-root "$V$X86SIM_RUNTIME" -cache_dir "$V$OUT" -arch x86_64 -force \
	-dylib_order_file "$V$RES/dylib-order.txt" -dirty_data_order_file "$V$RES/dirty-data-segments-order.txt" \
	> "$OUT/stdout.txt" 2> "$OUT/stderr.txt" < /dev/null
DPREFIX="$P" "$X86SIM_DARLING" shutdown >/dev/null 2>&1 || true
trap - EXIT
[ -f "$OUT/dyld_sim_shared_cache_x86_64" ] || { echo "build failed, see $OUT/stderr.txt" >&2; tail -5 "$OUT/stderr.txt" >&2; exit 1; }
python3 "$X/cli/libexec/dyldcache.py" localize "$OUT"
[ -n "$REF" ] && python3 "$X/cli/libexec/dyldcache.py" compare "$OUT" "$REF"
case $P in "${X86SIM_SCRATCH:-$HOME/scratch/x86code}"/*) rm -rf "$P" ;; esac
