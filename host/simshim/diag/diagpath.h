// x86Sim: where the diagnostic shims write their logs and read their filters (oslog.filter, oslog.bt): the device's
// data directory as seen inside Darling (/Volumes/SystemRoot + $X86SIM_DEVICE/data). build/mac-build-diag.sh passes
// it with -DX86SIM_DATA_DIR=...; the fallback is the container's /private/tmp.
#ifndef X86SIM_DATA_DIR
#define X86SIM_DATA_DIR "/private/tmp"
#endif

// x86Sim (RAM 5): the data directory at run time, so one build serves every phone. Daemons run with HOME (and
// CFFIXED_USER_HOME) = the device's data directory (mksimdevice.py child_defaults); an app's HOME is its container
// below it (<data>/Containers/...), cut there. Neither set: the build-time X86SIM_DATA_DIR.
#include <stdlib.h>
#include <string.h>
static inline const char* diag_data_dir(void)
{
	static char dir[1024];
	if (!dir[0]) {
		const char* h = getenv("CFFIXED_USER_HOME");
		if (!h || !*h) h = getenv("HOME");
		if (h && *h && strlen(h) < sizeof(dir)) {
			strcpy(dir, h);
			char* c = strstr(dir, "/Containers/");
			if (c) *c = 0;
		} else
			strcpy(dir, X86SIM_DATA_DIR);
	}
	return dir;
}
