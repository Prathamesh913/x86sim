// x86Code: for each pid, fetch the entitlements blob the way libxpc does
// (csops op 7 with a 0x408 buffer, ERANGE -> retry with the header's length)
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
int proc_pidpath(int pid, void* buf, uint32_t size);
int csops(int pid, unsigned int ops, void* useraddr, size_t usersize);

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	sleep(10);
	for (int pid = 1; pid < 80; pid++) {
		char path[1024] = "";
		if (proc_pidpath(pid, path, sizeof(path)) <= 0)
			continue;
		uint8_t* buf = malloc(0x408);
		int r = csops(pid, 7, buf, 0x408);
		int e = errno;
		uint32_t len = __builtin_bswap32(*(uint32_t*)(buf + 4));
		printf("%d %s: first r=%d errno=%d len=%u", pid, strrchr(path, '/') + 1, r, r ? e : 0, len);
		if (r < 0 && e == ERANGE) {
			buf = realloc(buf, len);
			r = csops(pid, 7, buf, len);
			printf(" | second r=%d errno=%d magic=%08x has_system_container=%d", r, r ? errno : 0,
				__builtin_bswap32(*(uint32_t*)buf), r == 0 && memmem(buf, len, "system-container", 16) != NULL);
		}
		printf("\n");
		free(buf);
	}
	return 0;
}
