#!/bin/bash
# x86Code RAM 3: build + run the fastmem checked-vs-unchecked test against the libarm64jit build's libdynarmic.a.
set -e
B=${B:-$HOME/scratch/x86code/arm64/build-arm64jit}
D=$(cd "$(dirname "$0")/../../../external/dynarmic" && pwd)
O=${O:-$HOME/scratch/x86code/fastmemtest}
g++ -std=c++20 -O1 -I"$D/src" -I"$D/externals/mcl/include" -I"$D/externals/fmt/include" -I"$D/externals/xbyak" \
  "$(dirname "$0")/fastmemtest.cpp" "$B/dynarmic/src/dynarmic/libdynarmic.a" \
  $(find "$B/dynarmic/externals" -name '*.a' | sort) -o "$O"
"$O"
