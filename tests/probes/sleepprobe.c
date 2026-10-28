// x86Code: timed-wait soak probe (usleep = __semwait_signal = darlingserver semaphore_timedwait). Run as a job:
//   sleepprobe=tests/probes/sleepprobe|env:SLEEPPROBE_SECS=1200
// 6 threads sleep 1-300 ms in a loop; each logs every 20 s and any sleep that took 1 s longer than asked.
// Log: /private/var/tmp/sleepprobe.log
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <mach/mach_time.h>

static FILE* out;
static double t0;
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static void* run(void* arg) {
	long id = (long) arg;
	unsigned seed = (unsigned) id * 7919;
	long n = 0;
	double last = now();
	for (;;) {
		int us = 1000 + rand_r(&seed) % 300000;
		double a = now();
		if (id == 0) { struct timespec ts = { 0, us * 1000L }; nanosleep(&ts, NULL); } else usleep(us);
		double took = now() - a;
		n++;
		if (took > us / 1e6 + 1.0) { fprintf(out, "%8.3f T%ld: sleep %d us took %.3f s\n", now() - t0, id, us, took); fflush(out); }
		if (now() - last > 20) { last = now(); fprintf(out, "%8.3f T%ld: %ld sleeps\n", now() - t0, id, n); fflush(out); }
	}
	return NULL;
}

int main(void) {
	out = fopen("/private/var/tmp/sleepprobe.log", "a");
	t0 = now();
	fprintf(out, "start\n"); fflush(out);
	const char* e = getenv("SLEEPPROBE_SECS");
	for (long i = 0; i < 6; i++) { pthread_t t; pthread_create(&t, NULL, run, (void*) i); }
	sleep(e ? atoi(e) : 1200);
	fprintf(out, "end\n");
	return 0;
}
