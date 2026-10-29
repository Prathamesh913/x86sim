#!/bin/sh
# x86Code: build the x86_64 dyld shared cache of the iOS 26.5 (23F73) simulator runtime on the Mac ($X86SIM_MAC) with the
# runtime's own update_dyld_sim_shared_cache, copy it once to runtimes/dyld-cache-23F73-x86_64/ (~4.4 GB, rsync at
# 40 MB/s, low IO priority), verify the checksums, then localize the image table (cli/libexec/dyldcache.py) and remove
# the Mac copy. The Mac's own CoreSimulator cache for this runtime is arm64 only. See docs/notebook/FINDINGS.md (dyld shared cache).
#   build/mac-build-dyldcache.sh
set -e
. "$(dirname "$0")/mac-env.sh"
OUT=$X/runtimes/dyld-cache-23F73-x86_64
[ -f "$OUT/dyld_sim_shared_cache_x86_64" ] && { echo "$OUT already has a cache (remove it to rebuild)" >&2; exit 1; }
mac 'set -e
R=$(xcrun simctl runtime list -v | awk "/\\(23F73\\)/{f=1} f&&/Bundle Path:/{sub(/.*Bundle Path: /,\"\"); print; exit}")
[ -n "$R" ] || { echo "23F73 runtime not mounted" >&2; exit 1; }
B="$R/Contents/Resources"; O=~/scratch/x86cache; rm -rf $O; mkdir -p $O
"$B/update_dyld_sim_shared_cache" -root "$B/RuntimeRoot" -cache_dir $O -arch x86_64 -force \
	-dylib_order_file "$B/dylib-order.txt" -dirty_data_order_file "$B/dirty-data-segments-order.txt" \
	> $O/stdout.txt 2> $O/stderr.txt
cd $O && shasum -a 256 dyld_sim_shared_cache_x86_64* > SHA256SUMS'
mkdir -p "$OUT"
ionice -c3 nice -n19 rsync -a --bwlimit=40000 "$MAC:scratch/x86cache/" "$OUT/"
(cd "$OUT" && ionice -c3 nice -n19 sha256sum -c SHA256SUMS)
python3 "$X/cli/libexec/dyldcache.py" localize "$OUT"
mac 'rm -rf ~/scratch/x86cache'
