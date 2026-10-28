#include <stdio.h>
#include <unistd.h>
#include <mach/mach.h>

int main(void) {
    printf("hello from iossimulator x86_64, pid %d, task port %u\n", getpid(), mach_task_self());
    return 0;
}
