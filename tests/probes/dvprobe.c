// x86Code: does dyld_get_image_versions work on a malloc'd copy of a Mach-O header (as LaunchServices'
// _LSParseLoadCommands uses it to find LSExecutablePlatformKey)?
#include "probe_paths.h" // x86Code: X86CODE_RUNTIME_ROOT
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
extern void dyld_get_image_versions(const struct mach_header* mh, void (^cb)(uint32_t platform, uint32_t sdk, uint32_t minos));
extern int dyld_get_active_platform(void);
extern const struct mach_header* _NSGetMachExecuteHeader(void);
int main(int argc, char** argv) {
	printf("active %d\n", dyld_get_active_platform());
	printf("own image:\n");
	dyld_get_image_versions((const struct mach_header*) _dyld_get_image_header(0), ^(uint32_t p, uint32_t s, uint32_t m) { printf("  platform %u sdk %x minos %x\n", p, s, m); });
	const char* path = getenv("PROBE_FILE"); if (!path) path = X86CODE_RUNTIME_ROOT "/Applications/Maps.app/Maps";
	int fd = open(path, O_RDONLY);
	char* buf = malloc(65536);
	ssize_t n = read(fd, buf, 65536);
	uint32_t magic = *(uint32_t*) buf;
	long off = 0;
	if (magic == 0xbebafeca) { // FAT_CIGAM
		uint32_t na = __builtin_bswap32(((uint32_t*) buf)[1]);
		for (uint32_t i = 0; i < na; i++) { uint32_t* a = (uint32_t*) (buf + 8 + i * 20); if (__builtin_bswap32(a[0]) == 0x01000007) off = __builtin_bswap32(a[2]); }
		lseek(fd, off, SEEK_SET); n = read(fd, buf, 65536);
	}
	printf("file %s (n %zd, slice %ld, magic %x):\n", path, n, off, *(uint32_t*) buf);
	dyld_get_image_versions((const struct mach_header*) buf, ^(uint32_t p, uint32_t s, uint32_t m) { printf("  platform %u sdk %x minos %x\n", p, s, m); });
	return 0;
}
