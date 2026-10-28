// x86Code: diagnostic shim (Maps "No Internet Connection" banner): logs GEONetworkObserver's path updates and its
// reachability every 5 s. X86CODE_INSERT=host/simshim/diag/libgeodiag.dylib, GEODIAG_PROGS (default Maps,geod).
// Log: data/var/log/geodiag.<prog>.<pid>.log
#import <Foundation/Foundation.h>
#include "diagpath.h" // x86Code: X86CODE_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <objc/runtime.h>
#include <objc/message.h>
#include <fcntl.h>
#include <unistd.h>
#include <mach-o/dyld.h>

static int logfd(void) {
	static int fd = -1;
	if (fd < 0) {
		char p[220];
		snprintf(p, sizeof(p), X86CODE_DATA_DIR "/var/log/geodiag.%s.%d.log", getprogname(), getpid());
		fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	return fd;
}
#define L(...) dprintf(logfd(), __VA_ARGS__)
static double now(void) { return CFAbsoluteTimeGetCurrent(); }

static IMP origUpdated, origInitMon;
static void hUpdated(id self, SEL s, id path) {
	L("%.3f _networkPathUpdated: %s\n", now(), [[path description] UTF8String]);
	((void(*)(id, SEL, id))origUpdated)(self, s, path);
	// (isNetworkReachable syncs onto the queue this runs on: not called here)
}
static void hInitMon(id self, SEL s) {
	L("%.3f _initializeNetworkMonitor\n", now());
	((void(*)(id, SEL))origInitMon)(self, s);
}

static void dumpObj(const char* what, id o) {
	Class c = object_getClass(o);
	unsigned n = 0;
	Ivar* iv = class_copyIvarList(c, &n);
	L("%.3f %s %s:\n", now(), what, class_getName(c));
	for (unsigned i = 0; i < n; i++) {
		const char* t = ivar_getTypeEncoding(iv[i]);
		char* base = (char*)(__bridge void*)o + ivar_getOffset(iv[i]);
		if (t[0] == '@') { id v = object_getIvar(o, iv[i]); L("    %s = %s\n", ivar_getName(iv[i]), v ? [[[v description] substringToIndex:MIN((NSUInteger)200, [[v description] length])] UTF8String] : "nil"); }
		else if (t[0] == 'B' || t[0] == 'c' || t[0] == 'C') L("    %s = %d\n", ivar_getName(iv[i]), *(unsigned char*)base);
		else if (t[0] == 'q' || t[0] == 'Q' || t[0] == 'l' || t[0] == 'L') L("    %s = %lld\n", ivar_getName(iv[i]), *(long long*)base);
		else if (t[0] == 'i' || t[0] == 'I') L("    %s = %d\n", ivar_getName(iv[i]), *(int*)base);
		else L("    %s (%s)\n", ivar_getName(iv[i]), t);
	}
	free(iv);
}
static IMP oUpdType;
static void hUpdType(id self, SEL s) {
	((void(*)(id, SEL))oUpdType)(self, s);
	dumpObj("after updateIndicatorType", self);
}
static void installMaps(void) {
	static int done;
	Class c = objc_getClass("StatusIndicatorManager");
	if (done || !c) return;
	done = 1;
	unsigned n = 0;
	Method* ms = class_copyMethodList(c, &n);
	for (unsigned i = 0; i < n; i++) L("  -[StatusIndicatorManager %s]\n", sel_getName(method_getName(ms[i])));
	free(ms);
	Method m = class_getInstanceMethod(c, sel_getUid("updateIndicatorType"));
	if (m) oUpdType = method_setImplementation(m, (IMP)hUpdType);
}

static void install(void) {
	installMaps();
	static int done;
	Class c = objc_getClass("GEONetworkObserver");
	if (done || !c) return;
	done = 1;
	Method m = class_getInstanceMethod(c, sel_getUid("_networkPathUpdated:"));
	if (m) origUpdated = method_setImplementation(m, (IMP)hUpdated);
	m = class_getInstanceMethod(c, sel_getUid("_initializeNetworkMonitor"));
	if (m) origInitMon = method_setImplementation(m, (IMP)hInitMon);
	L("%.3f hooks installed (%p %p)\n", now(), origUpdated, origInitMon);
	dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(0, 0));
	dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 5 * NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(t, ^{
		id o = ((id(*)(id, SEL))objc_msgSend)((id)c, sel_getUid("sharedNetworkObserver"));
		BOOL r = ((BOOL(*)(id, SEL))objc_msgSend)(o, sel_getUid("isNetworkReachable"));
		BOOL k = ((BOOL(*)(id, SEL))objc_msgSend)(o, sel_getUid("isNetworkConstrained"));
		L("%.3f poll: reachable %d constrained %d\n", now(), r, k);
	});
	dispatch_resume(t);
}
static void added(const struct mach_header* mh, intptr_t slide) { install(); }

__attribute__((constructor)) static void init(void) {
	const char* want = getenv("GEODIAG_PROGS") ?: "Maps,geod";
	if (!strstr(want, getprogname())) return;
	_dyld_register_func_for_add_image(added);
}
