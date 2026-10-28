// Spike 3.3 probe: csops entitlements of another process (spawns argv[1])
#include <spawn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
extern char** environ;
extern int csops(pid_t pid, unsigned int ops, void* useraddr, size_t usersize);
int main(int argc, char** argv)
{
	static char buf[65536];
	pid_t pid;
	char* args[] = { argv[1], NULL };
	if (posix_spawn(&pid, argv[1], NULL, NULL, args, environ) != 0) { perror("spawn"); return 1; }
	usleep(300000);
	errno = 0;
	int r = csops(pid, 7, buf, sizeof(buf));
	uint32_t len = __builtin_bswap32(*(uint32_t*)(buf + 4));
	printf("pid %d: csops ENTITLEMENTS r=%d errno=%d magic=%08x len=%u has-system-container=%d\n", pid, r, errno,
		__builtin_bswap32(*(uint32_t*)buf), len, r == 0 && memmem(buf, len, "system-container", 16) != NULL);
	kill(pid, SIGKILL);
	return 0;
}
