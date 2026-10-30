// Speed 9: dispatch_after on the global queues Swift Concurrency uses (cooperative, flag 0x4) vs the plain ones.
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <time.h>
#include <mach/mach_time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static dispatch_semaphore_t sem; static double t0;
static void fired(void* name) { printf("%-34s %.3f s (want 0.300)\n", (const char*)name, now() - t0); dispatch_semaphore_signal(sem); }
static void one(const char* name, dispatch_queue_t q, dispatch_time_t when) {
	if (!q) { printf("%-34s no queue\n", name); return; }
	t0 = now(); dispatch_after_f(when, q, (void*)name, fired);
	if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC))) printf("%-34s never fired\n", name);
}
int main(void) {
	sem = dispatch_semaphore_create(0);
	const uint64_t d = 300 * NSEC_PER_MSEC;
	one("global default, uptime", dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), dispatch_time(DISPATCH_TIME_NOW, d));
	one("global overcommit, uptime", dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0x2), dispatch_time(DISPATCH_TIME_NOW, d));
	one("global cooperative, uptime", dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0x4), dispatch_time(DISPATCH_TIME_NOW, d));
	one("global cooperative UI, uptime", dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0x4), dispatch_time(DISPATCH_TIME_NOW, d));
	// DISPATCH_MONOTONICTIME_NOW (continuous clock): bit 63 (bit 62 marks wall time in this encoding)
	one("global default, monotonic", dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), dispatch_time((1ull << 63), d));
	one("global cooperative, monotonic", dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0x4), dispatch_time((1ull << 63), d));
	one("global default, wall", dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), dispatch_walltime(NULL, d));
	// what Swift Concurrency passes: absolute deadlines (uptime: raw mach ticks; continuous: bit 63 | continuous ticks)
	one("absolute uptime", dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0x4), mach_absolute_time() + d);
	one("absolute continuous", dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0x4), (1ull << 63) | (mach_continuous_time() + d));
	// dispatch_source timer (Swift's path when a tolerance is given)
	t0 = now();
	dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0x4));
	dispatch_source_set_event_handler_f(src, fired); dispatch_set_context(src, (void*)"source timer, absolute uptime");
	dispatch_source_set_timer(src, mach_absolute_time() + d, DISPATCH_TIME_FOREVER, 0); dispatch_resume(src);
	if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC))) printf("source timer never fired\n");
	dispatch_source_cancel(src);
	return 0;
}
