// x86Code: time an XPC sync round trip to each named Mach service (run as a sim job)
#include <xpc/xpc.h>
extern xpc_connection_t xpc_connection_create_mach_service_x(const char*, dispatch_queue_t, uint64_t) __asm__("_xpc_connection_create_mach_service");
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

static void* watchdog(void* name) {
	sleep(5);
	printf("STILL BLOCKED after 5s: %s\n", (char*)name);
	return NULL;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	static const char* names[] = { "com.apple.notifyd.xpc", "com.x86code.no-such-service", "com.apple.diagnosticd",
		"com.apple.frontboard.systemappservices", "com.apple.coreservices.lsd", "com.apple.coreservices.quarantine-resolver",
		"com.apple.runningboard", NULL };
	for (int i = 0; names[i]; i++) {
		pthread_t t;
		pthread_create(&t, NULL, watchdog, (void*)names[i]);
		struct timespec a, b;
		clock_gettime(CLOCK_MONOTONIC, &a);
		xpc_connection_t c = xpc_connection_create_mach_service_x(names[i], NULL, 0);
		xpc_connection_set_event_handler(c, ^(xpc_object_t e) {});
		xpc_connection_resume(c);
		xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
		xpc_object_t r = xpc_connection_send_message_with_reply_sync(c, m);
		clock_gettime(CLOCK_MONOTONIC, &b);
		pthread_cancel(t);
		char* d = xpc_copy_description(r);
		printf("%s: %.3fs %.100s\n", names[i], (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9, d);
		xpc_connection_cancel(c);
	}
	return 0;
}
