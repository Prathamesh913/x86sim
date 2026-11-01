#!/bin/bash
# tests/arm64/gate/gates.sh [N] [PREFIX]: N (default 3) consecutive cold-boot gate runs; stops at the first failure.
d=$(dirname "$0"); n=${1:-3}; p=${2:-g}
for i in $(seq 1 "$n"); do "$d/gate.sh" "$p$i" || { echo "gate $p$i failed"; exit 1; }; done
echo "gates: $n consecutive clean"
