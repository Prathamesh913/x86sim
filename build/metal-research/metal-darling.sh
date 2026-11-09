#!/bin/sh
# Research prototype: run Apple's Metal toolchain (x86_64 slice, from the MetalToolchain cryptex) under Darling.
#   metal-darling.sh metal|metallib <args...>   (run from the directory holding the inputs; paths relative)
# Own Darling prefix (never the simulators'); shim supplies 2 symbols Darling's libSystem/libc++ lack.
set -e
X=/srv/projects/x86code; R=$X/build/metal-research; P=${METAL_DPREFIX:-$HOME/scratch/x86code/metal-prefix}
tool=$1; shift; V=/Volumes/SystemRoot
args=""; for a in "$@"; do args="$args '$(printf %s "$a" | sed "s/'/'\\\\''/g")'"; done
DPREFIX=$P "$X/darling-prefix/bin/darling" shell /bin/sh -c "cd '$V$PWD' && DYLD_FORCE_FLAT_NAMESPACE=1 \
DYLD_INSERT_LIBRARIES=$V$R/shim/libmetalshim.dylib exec $V$R/toolchain/bin/$tool $args"
