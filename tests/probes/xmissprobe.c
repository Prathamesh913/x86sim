// x86Code: sync XPC sends to a missing Mach service with plain and complex payloads
// (port in body, OOL memory, anonymous endpoint), SpringBoard -> systemstatus.publisher shape
#include <xpc/xpc.h>
#include <mach/mach.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
extern xpc_connection_t xpc_connection_create_mach_service_x(const char*, dispatch_queue_t, uint64_t) __asm__("_xpc_connection_create_mach_service");
extern xpc_connection_t xpc_connection_create_x(const char*, dispatch_queue_t) __asm__("_xpc_connection_create");
extern xpc_endpoint_t xpc_endpoint_create_x(xpc_connection_t) __asm__("_xpc_endpoint_create");
extern void xpc_dictionary_set_mach_send_x(xpc_object_t, const char*, mach_port_t) __asm__("_xpc_dictionary_set_mach_send");
static char big[256 * 1024];
static void* watchdog(void* k) { sleep(5); printf("STILL BLOCKED after 5s: kind %ld\n", (long)k); return NULL; }
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	static const char* kinds[] = { "plain", "small data", "big data (OOL)", "mach send", "endpoint", "endpoint+big", NULL };
	for (long k = 0; kinds[k]; k++) {
		pthread_t t; pthread_create(&t, NULL, watchdog, (void*)k);
		xpc_connection_t c = xpc_connection_create_mach_service_x("com.x86code.missing", NULL, 0);
		xpc_connection_set_event_handler(c, ^(xpc_object_t e) {});
		xpc_connection_resume(c);
		xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
		xpc_dictionary_set_uint64(m, "f", 33);
		if (k == 1) xpc_dictionary_set_data(m, "root", big, 200);
		if (k == 2 || k == 5) xpc_dictionary_set_data(m, "root", big, sizeof(big));
		if (k == 3) { mach_port_t p; mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p);
			mach_port_insert_right(mach_task_self(), p, p, MACH_MSG_TYPE_MAKE_SEND); xpc_dictionary_set_mach_send_x(m, "port", p); }
		if (k >= 4) { xpc_connection_t l = xpc_connection_create_x(NULL, NULL);
			xpc_connection_set_event_handler(l, ^(xpc_object_t e) {}); xpc_connection_resume(l);
			xpc_object_t ool = xpc_array_create(NULL, 0); xpc_array_append_value(ool, xpc_endpoint_create_x(l));
			xpc_dictionary_set_value(m, "ool", ool); }
		struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
		xpc_object_t r = xpc_connection_send_message_with_reply_sync(c, m);
		clock_gettime(CLOCK_MONOTONIC, &b); pthread_cancel(t);
		char* d = xpc_copy_description(r);
		printf("%s: %.3fs %.90s\n", kinds[k], (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9, d);
		xpc_connection_cancel(c);
	}
	return 0;
}
