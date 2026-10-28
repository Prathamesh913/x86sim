// x86Code: does the simulator's _sim_load_entitlements_blob (libsystem_sim_kernel) read an app's
// __TEXT,__entitlements here, and is its ERANGE (buffer too small) seen by the caller? InstallCoordination's
// _IXCopyEntitlementsForPath (lsd's app registration) asks with an 8-byte buffer first and expects -1/ERANGE.
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	int (*load)(const char*, void*, size_t) = dlsym(RTLD_DEFAULT, "_sim_load_entitlements_blob");
	const char* root = getenv("IPHONE_SIMULATOR_ROOT");
	char p[1024];
	snprintf(p, sizeof p, "%s/Applications/MobileSafari.app/MobileSafari", root ? root : "");
	const char* path = argc > 1 ? argv[1] : p;
	printf("dlsym %p, path %s\n", (void*)load, path);
	if (!load) return 1;
	unsigned char small[8];
	errno = ENOENT;
	int r = load(path, small, sizeof small);
	int e = errno;
	printf("small buffer: r=%d errno=%d (%s) header=%02x%02x%02x%02x len=%u\n", r, e, strerror(e), small[0], small[1], small[2], small[3],
		(unsigned)(small[4] << 24 | small[5] << 16 | small[6] << 8 | small[7]));
	size_t n = (size_t)(small[4] << 24 | small[5] << 16 | small[6] << 8 | small[7]);
	if (n > 8 && n < (1 << 24)) {
		char* big = calloc(1, n + 1);
		r = load(path, big, n);
		printf("full: r=%d, %.200s\n", r, big + 8);
	}
	return 0;
}
