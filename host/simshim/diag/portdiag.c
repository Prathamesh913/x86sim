// x86Sim: diagnostic shim: logs every destruction of a receive right (mach_port_destruct,
// mach_port_mod_refs(RECEIVE, <0), mach_port_destroy) with the caller's backtrace, to find who
// destroys a port that libdispatch still watches ("Unexpected EV_VANISHED").
#include "diagpath.h" // x86Sim: X86SIM_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <mach/mach.h>
#include <execinfo.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdlib.h>

static int logfd(void)
{
	static int fd = -1;
	if (fd < 0) {
		char p[200];
		snprintf(p, sizeof(p), X86SIM_DATA_DIR "/var/log/portdiag.%s.%d.log", getprogname(), getpid());
		fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	return fd;
}

#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };

static void bt(const char* what, mach_port_name_t name, long arg)
{
	char line[160];
	void* f[12];
	uint64_t tid = 0;
	pthread_threadid_np(NULL, &tid);
	int n = snprintf(line, sizeof(line), "portdiag[%d/%llu]: %s 0x%x (%ld)\n", getpid(), tid, what, name, arg);
	int fd = logfd();
	write(fd, line, n);
	backtrace_symbols_fd(f + 1, backtrace(f, 12) - 1, fd);
}

static kern_return_t d_destruct(ipc_space_t t, mach_port_name_t n, mach_port_delta_t d, mach_port_context_t g)
{ bt("destruct", n, d); return mach_port_destruct(t, n, d, g); }
static kern_return_t d_mod_refs(ipc_space_t t, mach_port_name_t n, mach_port_right_t r, mach_port_delta_t d)
{ if (r == MACH_PORT_RIGHT_RECEIVE && d < 0) bt("mod_refs(recv)", n, d); return mach_port_mod_refs(t, n, r, d); }
static kern_return_t d_destroy(ipc_space_t t, mach_port_name_t n)
{ bt("destroy", n, 0); return mach_port_destroy(t, n); }
DYLD_INTERPOSE(d_destruct, mach_port_destruct)
DYLD_INTERPOSE(d_mod_refs, mach_port_mod_refs)
DYLD_INTERPOSE(d_destroy, mach_port_destroy)
