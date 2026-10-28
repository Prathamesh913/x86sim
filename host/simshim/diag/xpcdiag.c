// Diagnostic shim for simulator processes: logs xpc_set_event() payloads
// (xpcproxy_sim reports its setup failures this way) to /private/tmp/x86code-xpcdiag.log
#include <xpc/xpc.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void xpc_set_event(const char* stream, const char* name, xpc_object_t descriptor);

#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };

static void diag_xpc_set_event(const char* stream, const char* name, xpc_object_t descriptor)
{
	char* desc = descriptor ? xpc_copy_description(descriptor) : NULL;
	char line[4096];
	int fd = open("/private/tmp/x86code-xpcdiag.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	int n = snprintf(line, sizeof(line), "[%d] xpc_set_event(%s, %s): %s\n", getpid(), stream, name ? name : "(null)", desc ? desc : "(null)");
	if (fd >= 0)
	{
		write(fd, line, n < (int) sizeof(line) ? n : (int) sizeof(line) - 1);
		close(fd);
	}
	free(desc);
	xpc_set_event(stream, name, descriptor);
}
DYLD_INTERPOSE(diag_xpc_set_event, xpc_set_event)
