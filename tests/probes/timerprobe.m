// x86Code: run-loop/dispatch timer soak probe (status-bar clock stops). Run as a job:
//   timerprobe=tests/probes/timerprobe|env:TIMERPROBE_SECS=900
// Logs to /private/var/tmp/timerprobe.log: every fire of a minute-boundary NSTimer (as the status-bar clock),
// every 30th fire of a 1 s NSTimer, a main-queue dispatch_after chain (5 s) and a global dispatch timer (1 s).
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>
#include <stdio.h>

static FILE* out;
static double t0;
static double now(void) { return [NSDate timeIntervalSinceReferenceDate]; }
#define LOG(...) do { fprintf(out, "%8.3f ", now() - t0); fprintf(out, __VA_ARGS__); fputc('\n', out); fflush(out); } while (0)

@interface P : NSObject
@property (strong) NSTimer* minute;
@property int ticks;
@end
@implementation P
- (void)armMinute {
	NSDate* d = [NSDate date];
	double s = floor(d.timeIntervalSinceReferenceDate / 60.0) * 60.0 + 60.0;
	self.minute = [[NSTimer alloc] initWithFireDate:[NSDate dateWithTimeIntervalSinceReferenceDate:s] interval:0 target:self selector:@selector(minuteFired:) userInfo:nil repeats:NO];
	[[NSRunLoop mainRunLoop] addTimer:self.minute forMode:NSRunLoopCommonModes];
	LOG("minute timer armed for +%.3f", s - d.timeIntervalSinceReferenceDate);
}
- (void)minuteFired:(NSTimer*)t {
	double late = now() - t.fireDate.timeIntervalSinceReferenceDate;
	LOG("minute timer fired, late %.3f s", late);
	[self armMinute];
}
- (void)tick:(NSTimer*)t {
	if (++self.ticks % 30 == 0) LOG("1s timer: %d fires", self.ticks);
}
@end

static void chain(int n) {
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		if (n % 6 == 0) LOG("main dispatch_after chain: %d", n);
		chain(n + 1);
	});
}

int main(int argc, char** argv) {
	@autoreleasepool {
		out = fopen("/private/var/tmp/timerprobe.log", "a");
		if (!out) out = stderr;
		t0 = now();
		const char* e = getenv("TIMERPROBE_SECS");
		double secs = e ? atof(e) : 900;
		LOG("start, %.0f s", secs);
		P* p = [P new];
		[p armMinute];
		[NSTimer scheduledTimerWithTimeInterval:1.0 target:p selector:@selector(tick:) userInfo:nil repeats:YES];
		chain(1);
		__block int g = 0;
		dispatch_source_t ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(0, 0));
		dispatch_source_set_timer(ds, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, 0);
		dispatch_source_set_event_handler(ds, ^{ if (++g % 30 == 0) LOG("global dispatch timer: %d", g); });
		dispatch_resume(ds);
		__block int w = 0;
		dispatch_source_t wall = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(0, 0));
		dispatch_source_set_timer(wall, dispatch_walltime(NULL, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, 0);
		dispatch_source_set_event_handler(wall, ^{ if (++w % 15 == 0) LOG("wall dispatch timer: %d", w); });
		dispatch_resume(wall);
		[[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:secs]];
		LOG("end");
	}
	return 0;
}
