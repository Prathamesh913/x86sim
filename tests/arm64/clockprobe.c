// Speed 9: the guest's clocks side by side. Swift Concurrency's deadlines come from clock_gettime(CLOCK_UPTIME_RAW /
// CLOCK_MONOTONIC_RAW) and must be in mach_absolute_time / mach_continuous_time's epoch and units for dispatch.
#include <stdio.h>
#include <time.h>
#include <mach/mach_time.h>
int main(void) {
	mach_timebase_info_data_t tb; mach_timebase_info(&tb);
	printf("timebase %u/%u\n", tb.numer, tb.denom);
	printf("mach_absolute_time    %llu\n", (unsigned long long)mach_absolute_time());
	printf("mach_continuous_time  %llu\n", (unsigned long long)mach_continuous_time());
	printf("mach_approximate_time %llu\n", (unsigned long long)mach_approximate_time());
	const struct { const char* n; clockid_t c; } cs[] = { {"CLOCK_REALTIME", CLOCK_REALTIME}, {"CLOCK_MONOTONIC", CLOCK_MONOTONIC},
		{"CLOCK_MONOTONIC_RAW", CLOCK_MONOTONIC_RAW}, {"CLOCK_MONOTONIC_RAW_APPROX", CLOCK_MONOTONIC_RAW_APPROX},
		{"CLOCK_UPTIME_RAW", CLOCK_UPTIME_RAW}, {"CLOCK_UPTIME_RAW_APPROX", CLOCK_UPTIME_RAW_APPROX} };
	for (int i = 0; i < 6; i++) {
		struct timespec ts = {0}; int r = clock_gettime(cs[i].c, &ts);
		printf("%-26s r=%d %lld.%09ld  nsec_np %llu\n", cs[i].n, r, (long long)ts.tv_sec, ts.tv_nsec, (unsigned long long)clock_gettime_nsec_np(cs[i].c));
	}
	printf("mach_absolute_time    %llu\n", (unsigned long long)mach_absolute_time());
	return 0;
}
