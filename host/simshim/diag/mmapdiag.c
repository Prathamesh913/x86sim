// x86Sim: diagnostic shim: logs failing mmap() calls (MercuryPosterExtension's vImageCreateCGImageFromBuffer failed
// with kvImageMemoryAllocationError) with errno and arguments, in MercuryPosterExtension only.
#include "diagpath.h" // x86Sim: X86SIM_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <execinfo.h>
#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };
static int logfd(void) {
	static int fd = -1;
	if (fd < 0) {
		char p[200];
		snprintf(p, sizeof(p), X86SIM_DATA_DIR "/var/log/mmapdiag.%s.%d.log", getprogname(), getpid());
		fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	return fd;
}
static void* my_mmap(void* a, size_t len, int prot, int flags, int fd, off_t off) {
	void* r = mmap(a, len, prot, flags, fd, off);
	if (r == MAP_FAILED && !strcmp(getprogname(), "MercuryPosterExtension")) {
		int e = errno;
		char line[300];
		int n = snprintf(line, sizeof(line), "mmap(%p, %zu, %d, %#x, %#x, %lld) failed: errno %d\n", a, len, prot, flags, fd, (long long)off, e);
		write(logfd(), line, n);
		void* bt[16]; int k = backtrace(bt, 16); backtrace_symbols_fd(bt, k, logfd());
		errno = e;
	}
	return r;
}
DYLD_INTERPOSE(my_mmap, mmap)
