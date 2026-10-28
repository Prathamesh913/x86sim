// x86Code: FSEvents latency probe (host/fseventsd). Run as a job:
//   x86sim boot "com.x86code.fsevprobe=tests/probes/fsevprobe|<directory inside the device data dir>"
// Streams on <dir> with latency 1.5 s (deferred) and 1.5 s + kFSEventStreamCreateFlagNoDefer, then creates files:
// the deferred stream must call back ~1.5 s after the first file, once for a burst; the NoDefer stream at once for
// the first file. A third stream (latency 30 s) checks that FSEventStreamFlushSync delivers pending events.
// Log: <data>/var/log/x86code-fsevprobe.log (lines "PASS"/"FAIL").
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <sys/stat.h>
// FSEvents isn't in the iOS SDK headers; the runtime has the framework (private): declared here, looked up at run time
typedef struct __FSEventStream* FSEventStreamRef;
typedef const struct __FSEventStream* ConstFSEventStreamRef;
typedef UInt32 FSEventStreamCreateFlags, FSEventStreamEventFlags;
typedef UInt64 FSEventStreamEventId;
typedef struct { CFIndex version; void* info; const void* (*retain)(const void*); void (*release)(const void*); CFStringRef (*copyDescription)(const void*); } FSEventStreamContext;
typedef void (*FSEventStreamCallback)(ConstFSEventStreamRef, void*, size_t, void*, const FSEventStreamEventFlags*, const FSEventStreamEventId*);
#define kFSEventStreamEventIdSinceNow 0xFFFFFFFFFFFFFFFFULL
#define kFSEventStreamCreateFlagNoDefer 0x02
#define kFSEventStreamCreateFlagFileEvents 0x10
static FSEventStreamRef (*FSEventStreamCreate)(CFAllocatorRef, FSEventStreamCallback, FSEventStreamContext*, CFArrayRef, FSEventStreamEventId, CFTimeInterval, FSEventStreamCreateFlags);
static void (*FSEventStreamSetDispatchQueue)(FSEventStreamRef, dispatch_queue_t);
static Boolean (*FSEventStreamStart)(FSEventStreamRef);
static void (*FSEventStreamFlushSync)(FSEventStreamRef);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE* lf;
static double t0;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double first[3]; static int calls[3]; static int nev[3];

static void cb(ConstFSEventStreamRef s, void* info, size_t n, void* paths, const FSEventStreamEventFlags* fl, const FSEventStreamEventId* ids) {
	int k = (int) (long) info;
	if (!calls[k]) first[k] = now() - t0;
	calls[k]++; nev[k] += (int) n;
	fprintf(lf, "stream %d: callback at %.2f s, %zu event(s), first %s\n", k, now() - t0, n, ((char**) paths)[0]); fflush(lf);
}

static FSEventStreamRef mk(const char* dir, int k, double lat, FSEventStreamCreateFlags f, dispatch_queue_t q) {
	CFStringRef p = CFStringCreateWithCString(NULL, dir, kCFStringEncodingUTF8);
	CFArrayRef a = CFArrayCreate(NULL, (const void**) &p, 1, &kCFTypeArrayCallBacks);
	FSEventStreamContext c = { 0, (void*) (long) k, NULL, NULL, NULL };
	FSEventStreamRef s = FSEventStreamCreate(NULL, cb, &c, a, kFSEventStreamEventIdSinceNow, lat, f | kFSEventStreamCreateFlagFileEvents);
	FSEventStreamSetDispatchQueue(s, q);
	FSEventStreamStart(s);
	return s;
}

int main(int argc, char** argv) {
	char lp[1024];
	snprintf(lp, sizeof lp, "%s/var/log/x86code-fsevprobe.log", getenv("HOME") ?: "/tmp");
	lf = fopen(lp, "w");
	void* h = dlopen("/System/Library/PrivateFrameworks/FSEvents.framework/FSEvents", RTLD_NOW);
	if (!h) h = dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
	FSEventStreamCreate = dlsym(RTLD_DEFAULT, "FSEventStreamCreate");
	FSEventStreamSetDispatchQueue = dlsym(RTLD_DEFAULT, "FSEventStreamSetDispatchQueue");
	FSEventStreamStart = dlsym(RTLD_DEFAULT, "FSEventStreamStart");
	FSEventStreamFlushSync = dlsym(RTLD_DEFAULT, "FSEventStreamFlushSync");
	if (!FSEventStreamCreate) { fprintf(lf, "FAIL: no FSEventStreamCreate\n"); fflush(lf); for (;;) pause(); }
	const char* dir = argc > 1 ? argv[1] : "/tmp/fsevprobe";
	char d[1024]; snprintf(d, sizeof d, "%s/fsevprobe-%d", dir, getpid());
	mkdir(d, 0755);
	dispatch_queue_t q = dispatch_queue_create("fsev", NULL);
	FSEventStreamRef s0 = mk(d, 0, 1.5, 0, q);
	FSEventStreamRef s1 = mk(d, 1, 1.5, kFSEventStreamCreateFlagNoDefer, q);
	FSEventStreamRef s2 = mk(d, 2, 30.0, 0, q);
	sleep(2);
	t0 = now();
	for (int i = 0; i < 5; i++) {
		char f[1100]; snprintf(f, sizeof f, "%s/f%d", d, i);
		FILE* x = fopen(f, "w"); fputs("x", x); fclose(x);
		usleep(100000);
	}
	sleep(4);
	FSEventStreamFlushSync(s2);
	sleep(1);
	int ok0 = calls[0] >= 1 && first[0] >= 1.2 && first[0] < 3.0 && calls[0] <= 2;
	int ok1 = calls[1] >= 1 && first[1] < 0.8;
	int ok2 = calls[2] >= 1;
	fprintf(lf, "deferred (latency 1.5): first %.2f s, %d callback(s): %s\n", first[0], calls[0], ok0 ? "PASS" : "FAIL");
	fprintf(lf, "NoDefer: first %.2f s: %s\n", first[1], ok1 ? "PASS" : "FAIL");
	fprintf(lf, "flush (latency 30): %d callback(s): %s\n", calls[2], ok2 ? "PASS" : "FAIL");
	fflush(lf);
	(void) s0; (void) s1;
	for (;;) pause();
}
