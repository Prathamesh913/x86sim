// x86Code: how long darlingserver's timed waits take (sleep +1 s, Spike 3.10). Run as a job:
//   waitprobe=tests/probes/waitprobe
// Log: /private/var/tmp/waitprobe.log
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <mach/mach.h>
#include <mach/semaphore.h>
#include <mach/task.h>

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

int main(void) {
	FILE* out = fopen("/private/var/tmp/waitprobe.log", "a");
	semaphore_t sem;
	semaphore_create(mach_task_self(), &sem, 0, 0);
	mach_port_t port;
	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
	for (int round = 0; round < 3; round++) {
		int ms[] = { 1, 50, 100, 500, 1500 };
		for (int i = 0; i < 5; i++) {
			double a = now();
			mach_timespec_t ts = { ms[i] / 1000, (ms[i] % 1000) * 1000000 };
			kern_return_t kr = semaphore_timedwait(sem, ts);
			double b = now();
			struct { mach_msg_header_t h; char body[64]; } msg;
			mach_msg_return_t mr = mach_msg(&msg.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(msg), port, ms[i], MACH_PORT_NULL);
			double c = now();
			struct timespec rq = { ms[i] / 1000, (ms[i] % 1000) * 1000000L };
			nanosleep(&rq, NULL);
			double d = now();
			fprintf(out, "%5d ms: semaphore_timedwait %.3f (kr %d)  mach_msg timeout %.3f (%x)  nanosleep %.3f\n", ms[i], b - a, kr, c - b, mr, d - c);
			fflush(out);
		}
	}
	return 0;
}
