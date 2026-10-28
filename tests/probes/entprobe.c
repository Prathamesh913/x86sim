// Spike 3.3 probe: simulator entitlements (__TEXT,__entitlements) as other processes see them
#include <spawn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <xpc/xpc.h>
extern char** environ;
extern int csops(pid_t pid, unsigned int ops, void* useraddr, size_t usersize);
extern xpc_object_t xpc_copy_entitlement_for_self(const char* key);
static void show(const char* who, pid_t pid)
{
	static char buf[65536];
	errno = 0;
	int r = csops(pid, 7, buf, sizeof(buf));
	uint32_t len = __builtin_bswap32(*(uint32_t*)(buf + 4));
	printf("%s pid %d: csops(7) r=%d errno=%d magic=%08x len=%u system-container=%d\n", who, pid, r, errno,
		__builtin_bswap32(*(uint32_t*)buf), len, r == 0 && memmem(buf, len, "system-container", 16) != NULL);
}
int main(int argc, char** argv)
{
	if (argc > 1) { sleep(5); return 0; } // child
	show("self", getpid());
	xpc_object_t v = xpc_copy_entitlement_for_self("com.apple.security.system-container");
	printf("self xpc_copy_entitlement_for_self -> %s\n", v ? (v == XPC_BOOL_TRUE ? "true" : "other") : "NULL");
	pid_t pid;
	char* args[] = { argv[0], "child", NULL };
	if (posix_spawn(&pid, argv[0], NULL, NULL, args, environ) != 0) { perror("spawn"); return 1; }
	usleep(500000);
	show("child", pid);
	kill(pid, SIGKILL);
	fflush(stdout);
	return 0;
}
