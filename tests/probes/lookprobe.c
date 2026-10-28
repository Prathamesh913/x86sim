// x86Code: time bootstrap_look_up for each argument (run as a sim job)
#include <mach/mach.h>
extern mach_port_t bootstrap_port;
kern_return_t bootstrap_look_up(mach_port_t, const char*, mach_port_t*);
#include <mach/mach.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

static void* watchdog(void* name) {
	sleep(5);
	printf("STILL BLOCKED after 5s: %s\n", (char*)name);
	fflush(stdout);
	return NULL;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	static char* names[] = { "lookprobe", "com.apple.notifyd", "com.x86code.no-such-service", "com.apple.diagnosticd",
		"com.apple.frontboard.systemappservices", "com.apple.coreservices.lsd", "com.apple.backboard.system-app-server", NULL };
	if (argc == 1) { argv = names; for (argc = 0; names[argc]; argc++); }
	for (int i = 1; i < argc; i++) {
		pthread_t t;
		pthread_create(&t, NULL, watchdog, argv[i]);
		struct timespec a, b;
		clock_gettime(CLOCK_MONOTONIC, &a);
		mach_port_t p = MACH_PORT_NULL;
		kern_return_t kr = bootstrap_look_up(bootstrap_port, argv[i], &p);
		clock_gettime(CLOCK_MONOTONIC, &b);
		pthread_cancel(t);
		printf("%s: kr=%d port=0x%x %.3fs\n", argv[i], kr, p, (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9);
	}
	return 0;
}
