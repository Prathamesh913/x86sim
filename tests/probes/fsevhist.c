// x86Code: FSEvents history probe (host/fseventsd event history across boots). Run as a job:
//   x86sim boot "com.x86code.fsevhist=tests/probes/fsevhist|<directory inside the device data dir>"
// First boot (no <dir>/fsevhist.id): records the current event id, starts a stream since that id on <dir>/h (so
// the daemon journals that tree), creates <dir>/h/during-boot1. Then, with the device shut down, tests/fsevhist.sh
// changes the tree from Linux (creates between-boots, modifies during-boot1). Next boot: a stream since the recorded
// id must get during-boot1 (created), between-boots (created) and the modification, then HistoryDone, without
// MustScanSubDirs. Log: <data>/var/log/x86code-fsevhist.log (lines "PASS"/"FAIL").
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct __FSEventStream* FSEventStreamRef;
typedef const struct __FSEventStream* ConstFSEventStreamRef;
typedef UInt32 FSEventStreamCreateFlags, FSEventStreamEventFlags;
typedef UInt64 FSEventStreamEventId;
typedef struct { CFIndex version; void* info; const void* (*retain)(const void*); void (*release)(const void*); CFStringRef (*copyDescription)(const void*); } FSEventStreamContext;
typedef void (*FSEventStreamCallback)(ConstFSEventStreamRef, void*, size_t, void*, const FSEventStreamEventFlags*, const FSEventStreamEventId*);
#define kFSEventStreamCreateFlagFileEvents 0x10
#define F_MUSTSCAN 0x1
#define F_HISTORYDONE 0x10
#define F_CREATED 0x100
#define F_REMOVED 0x200
#define F_MODIFIED 0x1000
static FSEventStreamRef (*FSEventStreamCreate)(CFAllocatorRef, FSEventStreamCallback, FSEventStreamContext*, CFArrayRef, FSEventStreamEventId, CFTimeInterval, FSEventStreamCreateFlags);
static void (*FSEventStreamSetDispatchQueue)(FSEventStreamRef, dispatch_queue_t);
static Boolean (*FSEventStreamStart)(FSEventStreamRef);
static void (*FSEventStreamFlushSync)(FSEventStreamRef);
static FSEventStreamEventId (*FSEventsGetCurrentEventId)(void);

static FILE* lf;
static volatile int done, mustscan, created1, created2, modified1, nev;

static void cb(ConstFSEventStreamRef s, void* info, size_t n, void* paths, const FSEventStreamEventFlags* fl, const FSEventStreamEventId* ids) {
	for (size_t i = 0; i < n; i++) {
		const char* p = ((char**) paths)[i];
		fprintf(lf, "event %llu flags %#x %s\n", (unsigned long long) ids[i], (unsigned) fl[i], p);
		if (fl[i] & F_HISTORYDONE) { done = 1; continue; }
		nev++;
		if (fl[i] & F_MUSTSCAN) mustscan = 1;
		if (strstr(p, "/during-boot1") && (fl[i] & F_CREATED)) created1 = 1;
		if (strstr(p, "/during-boot1") && (fl[i] & F_MODIFIED)) modified1 = 1;
		if (strstr(p, "/between-boots") && (fl[i] & F_CREATED)) created2 = 1;
	}
	fflush(lf);
}

static FSEventStreamRef mk(const char* dir, FSEventStreamEventId since, dispatch_queue_t q) {
	CFStringRef p = CFStringCreateWithCString(NULL, dir, kCFStringEncodingUTF8);
	CFArrayRef a = CFArrayCreate(NULL, (const void**) &p, 1, &kCFTypeArrayCallBacks);
	FSEventStreamContext c = { 0, NULL, NULL, NULL, NULL };
	FSEventStreamRef s = FSEventStreamCreate(NULL, cb, &c, a, since, 0.2, kFSEventStreamCreateFlagFileEvents);
	FSEventStreamSetDispatchQueue(s, q);
	FSEventStreamStart(s);
	return s;
}

int main(int argc, char** argv) {
	char lp[1024];
	snprintf(lp, sizeof lp, "%s/var/log/x86code-fsevhist.log", getenv("HOME") ?: "/tmp");
	lf = fopen(lp, "a");
	dlopen("/System/Library/PrivateFrameworks/FSEvents.framework/FSEvents", RTLD_NOW);
	dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
	FSEventStreamCreate = dlsym(RTLD_DEFAULT, "FSEventStreamCreate");
	FSEventStreamSetDispatchQueue = dlsym(RTLD_DEFAULT, "FSEventStreamSetDispatchQueue");
	FSEventStreamStart = dlsym(RTLD_DEFAULT, "FSEventStreamStart");
	FSEventStreamFlushSync = dlsym(RTLD_DEFAULT, "FSEventStreamFlushSync");
	FSEventsGetCurrentEventId = dlsym(RTLD_DEFAULT, "FSEventsGetCurrentEventId");
	if (!FSEventStreamCreate || !FSEventsGetCurrentEventId) { fprintf(lf, "FAIL: no FSEvents API\n"); fflush(lf); for (;;) pause(); }
	const char* dir = argc > 1 ? argv[1] : "/tmp/fsevhist";
	char h[1024], idf[1024], f1[1100];
	snprintf(h, sizeof h, "%s/h", dir);
	snprintf(idf, sizeof idf, "%s/fsevhist.id", dir);
	snprintf(f1, sizeof f1, "%s/during-boot1", h);
	mkdir(dir, 0755); mkdir(h, 0755);
	dispatch_queue_t q = dispatch_queue_create("fsev", NULL);
	FILE* f = fopen(idf, "r");
	unsigned long long since = 0;
	if (f) { if (fscanf(f, "%llu", &since) != 1) since = 0; fclose(f); }
	if (!since) {
		FSEventStreamEventId cur = FSEventsGetCurrentEventId();
		FSEventStreamRef s = mk(h, cur, q);
		sleep(1);
		FILE* x = fopen(f1, "w"); fputs("one", x); fclose(x);
		sleep(1);
		FSEventStreamFlushSync(s);
		f = fopen(idf, "w"); fprintf(f, "%llu\n", (unsigned long long) cur); fclose(f);
		fprintf(lf, "boot 1: recorded event id %llu, stream since it on %s, created during-boot1 (live events %d): %s\n",
			(unsigned long long) cur, h, nev, nev ? "PASS" : "FAIL");
		fflush(lf);
		for (;;) pause();
	}
	fprintf(lf, "boot 2: stream since %llu on %s\n", since, h); fflush(lf);
	mk(h, since, q);
	for (int i = 0; i < 100 && !done; i++) usleep(100000);
	fprintf(lf, "history done: %s\n", done ? "PASS" : "FAIL");
	fprintf(lf, "during-boot1 created (journal): %s\n", created1 ? "PASS" : "FAIL");
	fprintf(lf, "between-boots created (catch-up): %s\n", created2 ? "PASS" : "FAIL");
	fprintf(lf, "during-boot1 modified between boots: %s\n", modified1 ? "PASS" : "FAIL");
	fprintf(lf, "no MustScanSubDirs: %s\n", !mustscan ? "PASS" : "FAIL");
	fflush(lf);
	unlink(idf);
	for (;;) pause();
}
