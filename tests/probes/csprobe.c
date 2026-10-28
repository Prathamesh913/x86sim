// x86Code: print CS_OPS_STATUS (csops and SecTaskGetCodeSignStatus) for pids 30..45
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <Security/Security.h>
typedef struct __SecTask* SecTaskRef;
int csops(int pid, unsigned ops, void* a, size_t n);
extern uint32_t SecTaskGetCodeSignStatus(SecTaskRef);
extern SecTaskRef SecTaskCreateWithAuditToken(CFAllocatorRef, audit_token_t);
int main(void) {
	sleep(20);
	for (int pid = 30; pid < 45; pid++) {
		unsigned s = 0; int r = csops(pid, 0, &s, 4);
		audit_token_t t = {{0}}; t.val[5] = pid;
		SecTaskRef task = SecTaskCreateWithAuditToken(NULL, t);
		printf("pid %d csops %d 0x%x sectask 0x%x\n", pid, r, s, task ? SecTaskGetCodeSignStatus(task) : 0xdead);
	}
	return 0;
}
