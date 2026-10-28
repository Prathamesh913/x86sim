// x86Code: a thread blocked in mach_msg receive on a port another thread destroys
// (libdispatch's sync-reply waiter when the channel disconnects) must wake (MACH_RCV_PORT_DIED)
#include <mach/mach.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
extern kern_return_t mach_port_destruct(ipc_space_t, mach_port_name_t, mach_port_delta_t, mach_port_context_t);
static mach_port_t port;
static void* rcv(void* a) {
	struct { mach_msg_header_t h; char body[256]; } m;
	kern_return_t kr = mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof(m), port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	printf("  receive returned 0x%x\n", kr);
	return NULL;
}
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	for (int k = 0; k < 4; k++) {
		mach_port_options_t o = { .flags = k >= 2 ? MPO_CONTEXT_AS_GUARD | MPO_STRICT : 0 };
		mach_port_construct(mach_task_self(), &o, 0x1234, &port);
		pthread_t t; pthread_create(&t, NULL, rcv, NULL);
		usleep(300000);
		kern_return_t kr;
		if (k == 0) kr = mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
		else if (k == 1) kr = mach_port_destroy(mach_task_self(), port);
		else if (k == 2) kr = mach_port_destruct(mach_task_self(), port, 0, 0x1234);
		else kr = mach_port_unguard(mach_task_self(), port, 0x1234), kr |= mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
		printf("case %d: destroy kr 0x%x\n", k, kr);
		sleep(2);
		if (pthread_kill(t, 0) == 0) { printf("case %d: STILL BLOCKED after 2s\n", k); }
	}
	return 0;
}
