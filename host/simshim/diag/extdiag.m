// x86Sim: diagnostic shim (MobileSafari / any WebKit client): logs WebKit's BrowserEngineKit process launches
// (+[BEWebContentProcess/BENetworkingProcess/BERenderingProcess ...ProcessWith...:completion:]) and what their
// completions deliver. Log: data/var/log/extdiag.<prog>.<pid>.log
#import <Foundation/Foundation.h>
#include "diagpath.h" // x86Sim: X86SIM_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <objc/runtime.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <mach-o/dyld.h>

static int logfd(void) {
	static int fd = -1;
	if (fd < 0) {
		char p[220];
		snprintf(p, sizeof(p), X86SIM_DATA_DIR "/var/log/extdiag.%s.%d.log", getprogname(), getpid());
		fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	return fd;
}
#define L(...) dprintf(logfd(), __VA_ARGS__)
static double now(void) { return CFAbsoluteTimeGetCurrent(); }

#define N 6
static IMP orig[N];
static const char* names[N];
typedef void (^Done)(id, NSError*);

static Done wrap(int i, Done done) {
	double t0 = now();
	return [^(id p, NSError* e) {
		L("%.3f %s completion after %.3fs: proc=%s error=%s\n", now(), names[i], now() - t0, [[p description] UTF8String], [[e description] UTF8String]);
		done(p, e);
	} copy];
}
#define H3(i) static void h##i(id c, SEL s, id b, id ih, Done d) { L("%.3f %s bundle=%s\n", now(), names[i], [[b description] UTF8String]); ((void(*)(id, SEL, id, id, Done))orig[i])(c, s, b, ih, wrap(i, d)); }
#define H2(i) static void h##i(id c, SEL s, id ih, Done d) { L("%.3f %s\n", now(), names[i]); ((void(*)(id, SEL, id, Done))orig[i])(c, s, ih, wrap(i, d)); }
H3(0) H2(1) H3(2) H2(3) H3(4) H2(5)

static void hook(int i, const char* cls, const char* sel, IMP h) {
	Class c = objc_getClass(cls);
	Method m = c ? class_getClassMethod(c, sel_getUid(sel)) : NULL;
	names[i] = sel;
	if (!m) { L("no +[%s %s]\n", cls, sel); return; }
	orig[i] = method_setImplementation(m, h);
}

static void install(void) {
	static int done;
	if (done || !objc_getClass("BEWebContentProcess")) return;
	done = 1;
	hook(0, "BEWebContentProcess", "webContentProcessWithBundleID:interruptionHandler:completion:", (IMP)h0);
	hook(1, "BEWebContentProcess", "webContentProcessWithInterruptionHandler:completion:", (IMP)h1);
	hook(2, "BENetworkingProcess", "networkProcessWithBundleID:interruptionHandler:completion:", (IMP)h2);
	hook(3, "BENetworkingProcess", "networkProcessWithInterruptionHandler:completion:", (IMP)h3);
	hook(4, "BERenderingProcess", "renderingProcessWithBundleID:interruptionHandler:completion:", (IMP)h4);
	hook(5, "BERenderingProcess", "renderingProcessWithInterruptionHandler:completion:", (IMP)h5);
	L("%.3f hooks installed\n", now());
}
static void added(const struct mach_header* mh, intptr_t slide) { install(); }

__attribute__((constructor)) static void init(void) {
	const char* want = getenv("EXTDIAG_PROGS") ?: "MobileSafari";
	if (!strstr(want, getprogname())) return;
	_dyld_register_func_for_add_image(added);
}
