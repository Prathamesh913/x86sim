// x86Code: does DYLD_INSERT_LIBRARIES interposing work in the simulator?
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#ifdef SHIM
#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };
static pid_t my_getpid(void) { return 4242; }
DYLD_INTERPOSE(my_getpid, getpid)
#else
int main(void) {
	char buf[64];
	printf("getpid() direct = %d\n", getpid());
	// a call made from inside libc (cross-dylib from libsystem_c to libsystem_kernel)
	setvbuf(stdout, NULL, _IONBF, 0);
	snprintf(buf, sizeof(buf), "%d", getpid());
	printf("tmpnam-ish via libc: arc4random=%u\n", arc4random());
	return 0;
}
#endif
