// x86Sim: diagnostic shim: logs xpc_shmem_create(_readonly) (region info, checksum) and xpc_shmem_map
// (checksum of what the receiver sees), to compare sender and receiver bytes (gray icons).
#include "diagpath.h" // x86Sim: X86SIM_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <mach/mach.h>
kern_return_t mach_vm_region(vm_map_t, mach_vm_address_t*, mach_vm_size_t*, vm_region_flavor_t, vm_region_info_t, mach_msg_type_number_t*, mach_port_t*);
#include <xpc/xpc.h>
#include <execinfo.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
xpc_object_t xpc_shmem_create_readonly(const void*, size_t);
static int logfd(void) {
	static int fd = -1;
	if (fd < 0) {
		char p[200];
		snprintf(p, sizeof(p), X86SIM_DATA_DIR "/var/log/shmdiag.%s.%d.log", getprogname(), getpid());
		fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	return fd;
}
#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };
static unsigned long sum(const void* p, size_t n) {
	const unsigned char* c = p; unsigned long h = 5381;
	for (size_t i = 0; i < n; i++) h = h * 33 + c[i];
	return h;
}
static void logit(const char* what, const void* p, size_t n, xpc_object_t o) {
	char line[400];
	int len = snprintf(line, sizeof line, "%s %p n=%zu obj=%p sum=%lx sumhead=%lx\n", what, p, n, o, p ? sum(p, n) : 0, p ? sum(p, n < 4096 ? n : 4096) : 0);
	write(logfd(), line, len);
	if (getenv("SHMDIAG_BT")) { void* f[10]; backtrace_symbols_fd(f + 1, backtrace(f, 10) - 1, logfd()); }
}
static xpc_object_t d_ro(const void* p, size_t n) { xpc_object_t o = xpc_shmem_create_readonly(p, n); logit("create_ro", p, n, o); return o; }
static xpc_object_t d_cr(void* p, size_t n) { xpc_object_t o = xpc_shmem_create(p, n); logit("create", p, n, o); return o; }
static size_t d_map(xpc_object_t o, void** out) { size_t n = xpc_shmem_map(o, out); logit("map", n ? *out : NULL, n, o); return n; }
DYLD_INTERPOSE(d_ro, xpc_shmem_create_readonly)
DYLD_INTERPOSE(d_cr, xpc_shmem_create)
DYLD_INTERPOSE(d_map, xpc_shmem_map)
