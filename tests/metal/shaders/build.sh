#!/bin/sh
# x86Code: compile probe.metal on the Mac mini for the iOS 26.5 simulator (AIR + metallib), fetch the results.
# Outputs: probe.air (AIR bitcode), probe.metallib, probe.ll (disassembled AIR, for reading).
set -e
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}; . "$X86SIM_ROOT/build/mac-env.sh"
cd "$X/tests/metal/shaders"
TGT="-target air64-apple-ios26.5-simulator"
tar cf - probe.metal | mac "
rm -rf /tmp/vksh && mkdir -p /tmp/vksh && cd /tmp/vksh && tar xf - &&
xcrun -sdk iphonesimulator metal $TGT -c probe.metal -o probe.air >&2 &&
xcrun -sdk iphonesimulator metallib probe.air -o probe.metallib >&2 &&
(xcrun -sdk iphonesimulator metal-objdump -d probe.air > probe.ll 2>/dev/null || xcrun llvm-dis probe.air -o probe.ll 2>/dev/null || true) &&
tar cf - probe.air probe.metallib \$(ls probe.ll 2>/dev/null)" | tar xf -
ls -la
# the copies mtlprobe embeds (rebuild mtlprobe afterwards: build/mac-build-metal.sh)
xxd -i probe.metallib > probe_metallib.h
python3 -c "import json; open('probe_source.h','w').write('// generated from probe.metal by build.sh\nstatic const char probe_metal_source[] = ' + json.dumps(open('probe.metal').read()) + ';\n')"
