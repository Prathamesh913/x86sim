// x86Code: where the diagnostic shims write their logs and read their filters (oslog.filter, oslog.bt): the device's
// data directory as seen inside Darling (/Volumes/SystemRoot + $X86SIM_DEVICE/data). build/mac-build-diag.sh passes
// it with -DX86CODE_DATA_DIR=...; the fallback is the container's /private/tmp.
#ifndef X86CODE_DATA_DIR
#define X86CODE_DATA_DIR "/private/tmp"
#endif
