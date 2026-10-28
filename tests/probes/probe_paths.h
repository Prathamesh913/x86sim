// x86Code: the iOS runtime root as seen inside Darling (/Volumes/SystemRoot + $X86SIM_RUNTIME), for probes that open
// runtime files by default. Build with -DX86CODE_RUNTIME_ROOT='"/Volumes/SystemRoot/..."' or pass the path as argument.
#ifndef X86CODE_RUNTIME_ROOT
#define X86CODE_RUNTIME_ROOT "/Volumes/SystemRoot/opt/x86code/RuntimeRoot"
#endif
