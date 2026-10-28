#!/bin/bash
# Usage: host_imports.sh <RuntimeRoot> <outdir>
# Lists x86_64 symbols imported anywhere in the runtime but defined nowhere in it:
# these must come from the host libsystem_{kernel,platform,pthread} (or are weak/missing).
set -e
RT=$1; OUT=$2; mkdir -p "$OUT"
cd "$RT"
export TMPD=$(mktemp -d)
find . -type f \( -name '*.dylib' -o -perm -111 -o -path '*.framework/*' \) -size +4k -print0 \
 | xargs -0 -P"$(nproc)" -n 64 sh -c 'o=$(mktemp -p "$TMPD"); for f; do head -c4 "$f" | od -An -tx1 | grep -qE "ca fe ba be|cf fa ed fe" || continue; llvm-nm --arch=x86_64 -u "$f" 2>/dev/null | sed "s/^ *U //;s/^ *//" | sed "s|^|U |"; llvm-nm --arch=x86_64 -gU --defined-only "$f" 2>/dev/null | awk "{print \"D \" \$NF}"; done >"$o"' sh
cat "$TMPD"/* > "$OUT/all.txt"; rm -rf "$TMPD"
grep '^U ' "$OUT/all.txt" | cut -c3- | sort -u > "$OUT/undefined.txt"
grep '^D ' "$OUT/all.txt" | cut -c3- | sort -u > "$OUT/defined.txt"
comm -23 "$OUT/undefined.txt" "$OUT/defined.txt" > "$OUT/host_required.txt"
wc -l "$OUT"/undefined.txt "$OUT"/defined.txt "$OUT"/host_required.txt
