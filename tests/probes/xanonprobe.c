// x86Code: anonymous XPC listener + endpoint client in one process; sync and async
// messages, plain and complex (port right, large xpc_data), to find lost messages.
#include <xpc/xpc.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

extern xpc_connection_t xpc_connection_create(const char*, dispatch_queue_t) __asm__("_xpc_connection_create");
extern xpc_endpoint_t xpc_endpoint_create(xpc_connection_t) __asm__("_xpc_endpoint_create");
extern xpc_connection_t xpc_connection_create_from_endpoint(xpc_endpoint_t) __asm__("_xpc_connection_create_from_endpoint");
extern void xpc_dictionary_set_mach_send(xpc_object_t, const char*, mach_port_t) __asm__("_xpc_dictionary_set_mach_send");

static volatile int stage;
static void alrm(int s) { (void)s; printf("TIMEOUT at stage %d\n", stage); _exit(2); }

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGALRM, alrm);
	xpc_connection_t lis = xpc_connection_create(NULL, NULL);
	xpc_connection_set_event_handler(lis, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
		xpc_connection_set_event_handler(peer, ^(xpc_object_t m) {
			if (xpc_get_type(m) != XPC_TYPE_DICTIONARY) return;
			xpc_object_t r = xpc_dictionary_create_reply(m);
			if (r) { xpc_dictionary_set_int64(r, "ok", xpc_dictionary_get_int64(m, "f")); xpc_connection_send_message(peer, r); xpc_release(r); }
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(lis);
	xpc_endpoint_t ep = xpc_endpoint_create(lis);
	xpc_connection_t c = xpc_connection_create_from_endpoint(ep);
	xpc_connection_set_event_handler(c, ^(xpc_object_t e) { (void)e; });
	xpc_connection_resume(c);
	static char big[64 * 1024];
	memset(big, 'x', sizeof(big));
	for (int kind = 0; kind < 4; kind++) {
		for (int i = 0; i < 200; i++) {
			stage = kind * 1000 + i;
			alarm(5);
			xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
			xpc_dictionary_set_int64(m, "f", 33);
			if (kind == 1) xpc_dictionary_set_data(m, "root", big, 91);
			if (kind == 2) xpc_dictionary_set_data(m, "root", big, sizeof(big));
			if (kind == 3) xpc_dictionary_set_mach_send(m, "p", mach_task_self());
			xpc_object_t r = xpc_connection_send_message_with_reply_sync(c, m);
			if (xpc_get_type(r) != XPC_TYPE_DICTIONARY) { printf("kind %d i %d error reply\n", kind, i); }
			xpc_release(r); xpc_release(m);
		}
		printf("kind %d sync ok\n", kind);
		dispatch_semaphore_t s = dispatch_semaphore_create(0);
		alarm(10);
		for (int i = 0; i < 200; i++) {
			xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
			if (kind == 2) xpc_dictionary_set_data(m, "root", big, sizeof(big));
			if (kind == 3) xpc_dictionary_set_mach_send(m, "p", mach_task_self());
			xpc_connection_send_message_with_reply(c, m, NULL, ^(xpc_object_t r) { (void)r; dispatch_semaphore_signal(s); });
			xpc_release(m);
		}
		stage = kind * 1000 + 999;
		for (int i = 0; i < 200; i++) dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER);
		printf("kind %d async ok\n", kind);
	}
	printf("ALL OK\n");
	return 0;
}
