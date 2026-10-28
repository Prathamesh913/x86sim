// x86Code: anonymous XPC listener in a child process; the parent connects through the
// endpoint (lsd mapdb style) and sends sync/async messages, plain and complex, with
// plain and complex replies, to find lost messages.
#include <xpc/xpc.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

extern char** environ;
extern xpc_connection_t xpc_connection_create(const char*, dispatch_queue_t) __asm__("_xpc_connection_create");
extern xpc_endpoint_t xpc_endpoint_create(xpc_connection_t) __asm__("_xpc_endpoint_create");
extern xpc_connection_t xpc_connection_create_from_endpoint(xpc_endpoint_t) __asm__("_xpc_connection_create_from_endpoint");
extern void xpc_dictionary_set_mach_send(xpc_object_t, const char*, mach_port_t) __asm__("_xpc_dictionary_set_mach_send");
extern xpc_endpoint_t xpc_endpoint_create_mach_port_4sim(mach_port_t) __asm__("_xpc_endpoint_create_mach_port_4sim");
extern mach_port_t xpc_endpoint_copy_listener_port_4sim(xpc_endpoint_t) __asm__("_xpc_endpoint_copy_listener_port_4sim");

typedef struct { mach_msg_header_t h; mach_msg_body_t b; mach_msg_port_descriptor_t p; mach_msg_trailer_t t; } epmsg_t;
static volatile int stage;
static char big[64 * 1024];
static void alrm(int s) { (void)s; printf("TIMEOUT at stage %d\n", stage); _exit(2); }

static void add(xpc_object_t m, int kind)
{
	if (kind == 1) xpc_dictionary_set_data(m, "root", big, 91);
	if (kind == 2) xpc_dictionary_set_data(m, "root", big, sizeof(big));
	if (kind == 3) xpc_dictionary_set_mach_send(m, "p", mach_task_self());
}

static void serve(void)
{
	xpc_connection_t lis = xpc_connection_create(NULL, NULL);
	xpc_connection_set_event_handler(lis, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
		xpc_connection_set_event_handler(peer, ^(xpc_object_t m) {
			if (xpc_get_type(m) != XPC_TYPE_DICTIONARY) return;
			xpc_object_t r = xpc_dictionary_create_reply(m);
			if (r) {
				xpc_dictionary_set_int64(r, "ok", 1);
				add(r, (int)xpc_dictionary_get_int64(m, "rk"));
				xpc_connection_send_message(peer, r);
				xpc_release(r);
			}
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(lis);
	xpc_endpoint_t ep = xpc_endpoint_create(lis);
	mach_port_t* ports; mach_msg_type_number_t n = 0;
	mach_ports_lookup(mach_task_self(), &ports, &n);
	epmsg_t em; memset(&em, 0, sizeof(em));
	em.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
	em.h.msgh_size = sizeof(em) - sizeof(em.t);
	em.h.msgh_remote_port = ports[0];
	em.b.msgh_descriptor_count = 1;
	em.p.name = xpc_endpoint_copy_listener_port_4sim(ep);
	em.p.disposition = MACH_MSG_TYPE_MOVE_SEND;
	em.p.type = MACH_MSG_PORT_DESCRIPTOR;
	printf("server: n %u send %x\n", n, mach_msg(&em.h, MACH_SEND_MSG, em.h.msgh_size, 0, 0, 0, 0));
	dispatch_main();
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGALRM, alrm);
	memset(big, 'x', sizeof(big));
	if (getenv("XROLE")) serve();

	mach_port_t rp = MACH_PORT_NULL;
	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &rp);
	mach_port_insert_right(mach_task_self(), rp, rp, MACH_MSG_TYPE_MAKE_SEND);
	printf("register %x\n", mach_ports_register(mach_task_self(), &rp, 1));
	char path[1024]; uint32_t sz = sizeof(path); _NSGetExecutablePath(path, &sz);
	setenv("XROLE", "server", 1);
	pid_t pid; char* av[] = { path, NULL };
	printf("spawn %d\n", posix_spawn(&pid, path, NULL, NULL, av, environ));
	epmsg_t em; memset(&em, 0, sizeof(em));
	alarm(10);
	printf("recv %x\n", mach_msg(&em.h, MACH_RCV_MSG, 0, sizeof(em), rp, 0, 0));
	xpc_endpoint_t ep = xpc_endpoint_create_mach_port_4sim(em.p.name);

	for (int round = 0; round < 3; round++) {
		// a new anonymous connection per round, like lsd clients
		xpc_connection_t c = xpc_connection_create_from_endpoint(ep);
		xpc_connection_set_event_handler(c, ^(xpc_object_t e) { (void)e; });
		xpc_connection_resume(c);
		for (int kind = 0; kind < 4; kind++) for (int rk = 0; rk < 4; rk++) {
			for (int i = 0; i < 50; i++) {
				stage = round * 10000 + kind * 1000 + rk * 100 + i;
				alarm(5);
				xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
				xpc_dictionary_set_int64(m, "f", 33);
				xpc_dictionary_set_int64(m, "rk", rk);
				add(m, kind);
				xpc_object_t r = xpc_connection_send_message_with_reply_sync(c, m);
				if (xpc_get_type(r) != XPC_TYPE_DICTIONARY) printf("stage %d error reply\n", stage);
				xpc_release(r); xpc_release(m);
			}
			dispatch_semaphore_t s = dispatch_semaphore_create(0);
			stage = round * 10000 + kind * 1000 + rk * 100 + 99;
			alarm(10);
			for (int i = 0; i < 50; i++) {
				xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
				xpc_dictionary_set_int64(m, "rk", rk);
				add(m, kind);
				xpc_connection_send_message_with_reply(c, m, NULL, ^(xpc_object_t r) { (void)r; dispatch_semaphore_signal(s); });
				xpc_release(m);
			}
			for (int i = 0; i < 50; i++) dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER);
			printf("round %d kind %d rk %d ok\n", round, kind, rk);
		}
		xpc_connection_cancel(c);
	}
	printf("ALL OK\n");
	kill(pid, SIGTERM);
	return 0;
}
