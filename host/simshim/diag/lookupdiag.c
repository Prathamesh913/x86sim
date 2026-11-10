// x86Sim: diagnostic shim: who looks up which Mach/XPC service. Logs each distinct (process, service name) once to
// X86SIM_DATA_DIR/var/log/x86sim-lookups.log as "<epoch.ms> <process>[<pid>] <api> <name>", from
// xpc_connection_create_mach_service (NSXPCConnection, most clients), bootstrap_look_up/2/3 (CFMessagePort, MIG
// clients) and xpc_endpoint-less lookups that end up there. Pair with launchd's "Successfully spawned ... because
// ipc (mach)" lines to see what started an on-demand job (cli/libexec/svcwhy.py). X86SIM_INSERT=<this dylib>.
#include "diagpath.h"
#include <xpc/xpc.h>
#include <mach/mach.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };

// unavailable in the iOS SDK's header: declared under another C name
extern xpc_connection_t create_mach_service(const char* name, dispatch_queue_t q, uint64_t flags) __asm__("_xpc_connection_create_mach_service");
#define XPC_LISTENER (1 << 0)
extern kern_return_t bootstrap_look_up(mach_port_t bp, const char* name, mach_port_t* sp);
extern kern_return_t bootstrap_look_up2(mach_port_t bp, const char* name, mach_port_t* sp, pid_t pid, uint64_t flags);
extern kern_return_t bootstrap_look_up3(mach_port_t bp, const char* name, mach_port_t* sp, pid_t pid, const uuid_t uuid, uint64_t flags);

#define NSEEN 512
static unsigned long seen[NSEEN];
static int nseen;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static unsigned long hash(const char* s)
{
	unsigned long h = 1469598103934665603UL;
	for (; *s; s++) h = (h ^ (unsigned char) *s) * 1099511628211UL;
	return h | 1;
}

static void note(const char* api, const char* name)
{
	if (!name) return;
	unsigned long h = hash(name);
	pthread_mutex_lock(&mu);
	for (int i = 0; i < nseen; i++)
		if (seen[i] == h) { pthread_mutex_unlock(&mu); return; }
	if (nseen < NSEEN) seen[nseen++] = h;
	pthread_mutex_unlock(&mu);
	struct timeval tv;
	gettimeofday(&tv, NULL);
	char line[512];
	int n = snprintf(line, sizeof line, "%ld.%03d %s[%d] %s %s\n", (long) tv.tv_sec, (int) (tv.tv_usec / 1000),
		getprogname(), getpid(), api, name);
	if (n >= (int) sizeof line) n = sizeof line - 1;
	int fd = open(X86SIM_DATA_DIR "/var/log/x86sim-lookups.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) { write(fd, line, n); close(fd); }
}

static xpc_connection_t diag_create_mach_service(const char* name, dispatch_queue_t q, uint64_t flags)
{
	if (!(flags & XPC_LISTENER)) note("xpc", name);
	return create_mach_service(name, q, flags);
}
DYLD_INTERPOSE(diag_create_mach_service, create_mach_service)

static kern_return_t diag_look_up(mach_port_t bp, const char* name, mach_port_t* sp)
{
	note("bs", name);
	return bootstrap_look_up(bp, name, sp);
}
DYLD_INTERPOSE(diag_look_up, bootstrap_look_up)

static kern_return_t diag_look_up2(mach_port_t bp, const char* name, mach_port_t* sp, pid_t pid, uint64_t flags)
{
	note("bs2", name);
	return bootstrap_look_up2(bp, name, sp, pid, flags);
}
DYLD_INTERPOSE(diag_look_up2, bootstrap_look_up2)

static kern_return_t diag_look_up3(mach_port_t bp, const char* name, mach_port_t* sp, pid_t pid, const uuid_t uuid, uint64_t flags)
{
	note("bs3", name);
	return bootstrap_look_up3(bp, name, sp, pid, uuid, flags);
}
DYLD_INTERPOSE(diag_look_up3, bootstrap_look_up3)
