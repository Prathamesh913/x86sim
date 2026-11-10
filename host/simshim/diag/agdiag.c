// x86Sim: diagnostic shim (Speed 27, arm64): AttributeGraph update deadlines. Logs AGGraphSetDeadline (the deadline
// relative to now, in ms), every AGGraphCancelUpdateIfNeeded / AGGraphHasDeadlinePassed that returns true, with the
// overshoot. Why: Maps' route card aborted with "precondition failure: attribute failed to set an initial value"
// after AG::Graph::UpdateStack::cancel. Processes named in data/oslog.filter. Log: data/var/log/agdiag.<prog>.<pid>.log
#include "diagpath.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <mach/mach_time.h>
#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };

extern void AGGraphSetDeadline(void* graph, uint64_t deadline);
extern bool AGGraphHasDeadlinePassed(void);
extern bool AGGraphCancelUpdateIfNeeded(void);

static int en = -1, lfd = -1;
static uint64_t last_deadline;
static double ms_per_tick;
static int on(void)
{
	if (en < 0) {
		char buf[1024] = {0};
		int f = open(X86SIM_DATA_DIR "/oslog.filter", O_RDONLY);
		en = 0;
		if (f >= 0) {
			ssize_t n = read(f, buf, sizeof(buf) - 1); close(f);
			if (n > 0) {
				buf[strcspn(buf, "\n")] = 0;
				for (char* t = strtok(buf, ","); t; t = strtok(NULL, ",")) if (!strcmp(t, getprogname()) || !strcmp(t, "all")) en = 1;
			}
		}
		if (en) {
			char p[512];
			snprintf(p, sizeof(p), X86SIM_DATA_DIR "/var/log/agdiag.%s.%d.log", getprogname(), getpid());
			lfd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (lfd < 0) en = 0;
			mach_timebase_info_data_t tb; mach_timebase_info(&tb);
			ms_per_tick = (double) tb.numer / tb.denom / 1e6;
		}
	}
	return en;
}
static void lg(const char* fmt, ...)
{
	char b[400];
	int n = snprintf(b, sizeof(b), "%.3f [%llx] ", mach_absolute_time() * ms_per_tick / 1000.0,
		(unsigned long long) pthread_mach_thread_np(pthread_self()));
	va_list ap; va_start(ap, fmt); n += vsnprintf(b + n, sizeof(b) - n, fmt, ap); va_end(ap);
	if (n > (int) sizeof(b) - 2) n = sizeof(b) - 2;
	b[n++] = '\n'; write(lfd, b, n);
}
static void my_AGGraphSetDeadline(void* g, uint64_t d)
{
	if (on()) {
		uint64_t now = mach_absolute_time();
		if (d == UINT64_MAX) lg("SetDeadline none");
		else lg("SetDeadline in %.2f ms (deadline %llu now %llu)", ((double) d - (double) now) * ms_per_tick,
			(unsigned long long) d, (unsigned long long) now);
		last_deadline = d;
	}
	AGGraphSetDeadline(g, d);
}
DYLD_INTERPOSE(my_AGGraphSetDeadline, AGGraphSetDeadline)
static bool my_AGGraphHasDeadlinePassed(void)
{
	bool r = AGGraphHasDeadlinePassed();
	if (r && on()) lg("HasDeadlinePassed: yes (%.2f ms late)", ((double) mach_absolute_time() - (double) last_deadline) * ms_per_tick);
	return r;
}
DYLD_INTERPOSE(my_AGGraphHasDeadlinePassed, AGGraphHasDeadlinePassed)
static bool my_AGGraphCancelUpdateIfNeeded(void)
{
	bool r = AGGraphCancelUpdateIfNeeded();
	if (r && on()) lg("CancelUpdateIfNeeded: CANCELLED (%.2f ms past the deadline)", ((double) mach_absolute_time() - (double) last_deadline) * ms_per_tick);
	return r;
}
DYLD_INTERPOSE(my_AGGraphCancelUpdateIfNeeded, AGGraphCancelUpdateIfNeeded)
#include <execinfo.h>
#include <dlfcn.h>
extern void AGGraphCancelUpdate(void);
static void my_AGGraphCancelUpdate(void)
{
	if (on()) {
		lg("CancelUpdate called by:");
		void* f[12]; int k = backtrace(f, 12);
		for (int i = 1; i < k; i++) { Dl_info di; if (dladdr(f[i], &di) && di.dli_fname) lg("    %s %s+0x%lx", strrchr(di.dli_fname, '/') + 1, di.dli_sname ? di.dli_sname : "", (long) ((char*) f[i] - (char*) (di.dli_saddr ? di.dli_saddr : di.dli_fbase))); }
	}
	AGGraphCancelUpdate();
}
DYLD_INTERPOSE(my_AGGraphCancelUpdate, AGGraphCancelUpdate)
__attribute__((constructor)) static void agdiag_init(void) { if (on()) lg("agdiag loaded"); }
