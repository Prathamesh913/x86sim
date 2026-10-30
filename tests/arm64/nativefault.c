// Speed 5: a fault in native (Darling) code reached through a thunk must be reported and kill the process, not hang.
// strlen() of a bogus pointer faults inside Darling's native libsystem_platform. Expected: "fatal native fault" in the log, exit
// by SIGKILL within a few seconds.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    printf("nativefault: freeing a bogus pointer\n");
    fflush(stdout);
    volatile size_t n = strlen((const char *)(argc > 1 ? strtoul(argv[1], 0, 16) : 0x7f0000001000ul));
    printf("%zu\n", n);
    printf("nativefault: survived?\n");
    return 1;
}
