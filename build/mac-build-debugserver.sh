#!/bin/sh
# x86Code: build LLVM's debugserver (macOS x86_64) on the Mac ($X86SIM_MAC) for host/debugserver/debugserver.
# Xcode 26+ ships an arm64-only debugserver, so it is built from llvm-project's lldb/tools/debugserver Xcode project
# (sparse checkout, tag $LLVM_TAG) with MACOSX_DEPLOYMENT_TARGET=12.0: a newer target links libc++ symbols that
# Darling's /usr/lib/libc++.1.dylib doesn't have (std::bad_function_call's destructor). It runs inside the Darling
# container as a macOS process and attaches to simulator apps (x86sim debug). Everything on the Mac is removed after.
set -e
X86SIM_BUILD=mac   # Mac only: xcodebuild of the debugserver project
. "$(dirname "$0")/mac-env.sh"
LLVM_TAG=${LLVM_TAG:-llvmorg-21.1.0}
mac "set -e; rm -rf /tmp/x86dbg && mkdir -p /tmp/x86dbg && cd /tmp/x86dbg &&
git clone -q --depth 1 --filter=blob:none --sparse -b $LLVM_TAG https://github.com/llvm/llvm-project.git >&2 &&
cd llvm-project && git sparse-checkout set lldb/tools/debugserver lldb/include lldb/source/Host/common lldb/source/Utility >&2 &&
# x86Code: an app stopped at its first instruction (x86sim launch -w) has no dyld image list yet, so
# MachProcess::GetMainBinaryCPUTypes finds no main binary and debugserver picks a cpu type without a plugin in this
# x86_64-only build (it then crashed in MachThread::ThreadDidStop). Fall back to x86_64 (Darling has only 64-bit processes).
perl -0pi -e 's/(bool DNBArchProtocol::SetArchitecture\(uint32_t cpu_type, uint32_t cpu_subtype\) \{\n)/\1  if (g_arch_plugins.find(cpu_type) == g_arch_plugins.end() \&\& g_arch_plugins.find(CPU_TYPE_X86_64) != g_arch_plugins.end()) { cpu_type = CPU_TYPE_X86_64; cpu_subtype = CPU_SUBTYPE_X86_64_ALL; } \/\/ x86Code (Darling)\n/' lldb/tools/debugserver/source/DNBArch.cpp &&
grep -q 'x86Code (Darling)' lldb/tools/debugserver/source/DNBArch.cpp &&
cd lldb/tools/debugserver && xcodebuild -project debugserver.xcodeproj -target debugserver -configuration Release \
  -sdk macosx ARCHS=x86_64 ONLY_ACTIVE_ARCH=NO MACOSX_DEPLOYMENT_TARGET=12.0 CODE_SIGN_IDENTITY=- CODE_SIGNING_REQUIRED=NO \
  SYMROOT=/tmp/x86dbg/out >&2" 
mkdir -p "$X/host/debugserver"
mac 'cat /tmp/x86dbg/out/Release/debugserver' > "$X/host/debugserver/debugserver.new"
mac 'rm -rf /tmp/x86dbg'
[ -s "$X/host/debugserver/debugserver.new" ] && chmod +x "$X/host/debugserver/debugserver.new" &&
	mv "$X/host/debugserver/debugserver.new" "$X/host/debugserver/debugserver"
ls -la "$X/host/debugserver/debugserver"
