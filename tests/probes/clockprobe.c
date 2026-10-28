// x86Code: clock consistency (Spike 3.10): samples gettimeofday, clock_gettime(REALTIME/MONOTONIC) and
// mach_absolute_time for 3 s and reports any step against mach_absolute_time of more than 5 ms.
//   cli/libexec/run-sim.sh tests/probes/clockprobe
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <mach/mach_time.h>

int main(void) {
	mach_timebase_info_data_t tb; mach_timebase_info(&tb);
	double m0 = 0, g0 = 0, r0 = 0, c0 = 0;
	int bad = 0;
	for (long i = 0; i < 3000000 && bad < 20; i++) {
		struct timeval tv; gettimeofday(&tv, NULL);
		struct timespec rt, mt; clock_gettime(CLOCK_REALTIME, &rt); clock_gettime(CLOCK_MONOTONIC, &mt);
		double m = mach_absolute_time() * (double) tb.numer / tb.denom / 1e9;
		double g = tv.tv_sec + tv.tv_usec / 1e6, r = rt.tv_sec + rt.tv_nsec / 1e9, c = mt.tv_sec + mt.tv_nsec / 1e9;
		if (i) {
			double dm = m - m0;
			if (g - g0 - dm > 0.005 || g - g0 - dm < -0.005) { printf("gettimeofday step %+.6f (usec %ld)\n", g - g0 - dm, (long) tv.tv_usec); bad++; }
			if (r - r0 - dm > 0.005 || r - r0 - dm < -0.005) { printf("REALTIME step %+.6f (nsec %ld)\n", r - r0 - dm, rt.tv_nsec); bad++; }
			if (c - c0 - dm > 0.005 || c - c0 - dm < -0.005) { printf("MONOTONIC step %+.6f (sec %ld nsec %ld)\n", c - c0 - dm, (long) mt.tv_sec, mt.tv_nsec); bad++; }
		}
		m0 = m; g0 = g; r0 = r; c0 = c;
	}
	printf("done, %d steps\n", bad);
	return 0;
}
