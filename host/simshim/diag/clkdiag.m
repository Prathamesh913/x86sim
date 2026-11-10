// x86Sim: diagnostic shim (status-bar clock stops): logs SpringBoardUIServices' SBUIPreciseClockTimer display-link
// ticks (first 20, then every 50th) and frame-interval changes, and the display link's state every 10 s.
// X86SIM_INSERT=host/simshim/diag/libclkdiag.dylib, CLKDIAG_PROGS (default SpringBoard).
// Log: data/var/log/clkdiag.<prog>.<pid>.log
#import <Foundation/Foundation.h>
#include "diagpath.h" // x86Sim: X86SIM_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <objc/runtime.h>
#include <objc/message.h>
#include <fcntl.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>

static int logfd(void) {
	static int fd = -1;
	if (fd < 0) {
		char p[220];
		snprintf(p, sizeof(p), X86SIM_DATA_DIR "/var/log/clkdiag.%s.%d.log", getprogname(), getpid());
		fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	return fd;
}
#define L(...) dprintf(logfd(), __VA_ARGS__)
static double now(void) { return CFAbsoluteTimeGetCurrent(); }

static IMP oTick, oInterval, oUpdate, oPassed;
static long ticks;
static __weak id timerObj;
static void hTick(id self, SEL s, id link) {
	timerObj = self;
	if (++ticks <= 20 || ticks % 50 == 0) {
		id d = ((id(*)(id, SEL))objc_msgSend)(self, sel_getUid("date"));
		id n = ((id(*)(id, SEL))objc_msgSend)((id)object_getClass(self), sel_getUid("now"));
		L("%.3f tick %ld date=%s now=%s lastMinute=%ld\n", now(), ticks, [[d description] UTF8String], [[n description] UTF8String],
			*(long*)((char*)(__bridge void*)self + ivar_getOffset(class_getInstanceVariable(object_getClass(self), "_lastMinute"))));
	}
	((void(*)(id, SEL, id))oTick)(self, s, link);
}
static void hInterval(id self, SEL s, double secs) {
	((void(*)(id, SEL, double))oInterval)(self, s, secs);
}
static void hUpdate(id self, SEL s) {
	L("%.3f _updateDisplayLink\n", now());
	((void(*)(id, SEL))oUpdate)(self, s);
}
static void hPassed(id self, SEL s) {
	((void(*)(id, SEL))oPassed)(self, s);
}
static IMP oUpd, oRestart, oStop, oUpd0;
static void hUpd0(id self, SEL s) { L("%.3f SBStatusBarStateAggregator _updateTimeItems\n", now()); ((void(*)(id, SEL))oUpd0)(self, s); }
static void hUpd(id self, SEL s, id date) { L("%.3f SBStatusBarStateAggregator _updateTimeItemsWithDate: %s\n", now(), [[date description] UTF8String]); ((void(*)(id, SEL, id))oUpd)(self, s, date); }
static void hRestart(id self, SEL s) { L("%.3f SBStatusBarStateAggregator _restartTimeItemTimer\n", now()); ((void(*)(id, SEL))oRestart)(self, s); }
static void hStop(id self, SEL s) { L("%.3f SBStatusBarStateAggregator _stopTimeItemTimer\n", now()); ((void(*)(id, SEL))oStop)(self, s); }
static IMP oStart, oObserve;
typedef void (^MinuteHandler)(id);
static id hStart(id self, SEL s, MinuteHandler h) {
	L("%.3f startMinuteUpdatesWithHandler: %p\n", now(), h);
	MinuteHandler w = ^(id d) { L("%.3f minute handler(start) called: %s\n", now(), [[d description] UTF8String]); h(d); };
	id tok = ((id(*)(id, SEL, MinuteHandler))oStart)(self, s, w);
	L("%.3f   -> token %s\n", now(), [[tok description] UTF8String]);
	return tok;
}
struct BlockLayout { void* isa; int flags, reserved; void (*invoke)(void*, ...); };
static id hObserve(id self, SEL s, MinuteHandler h) {
	Dl_info di = {0};
	dladdr((void*)((__bridge struct BlockLayout*)h)->invoke, &di);
	L("%.3f observeMinuteUpdatesWithHandler: %p invoke %s\n", now(), h, di.dli_sname ?: "?");
	const char* nm = di.dli_sname;
	MinuteHandler w = ^(id d) { L("%.3f minute handler(observe %s) called\n", now(), nm ?: "?"); h(d); L("%.3f   handler returned\n", now()); };
	return ((id(*)(id, SEL, MinuteHandler))oObserve)(self, s, w);
}
static IMP oFire, oPObs;
static void hFire(id self, SEL s, id date) { L("%.3f SBFOverridableDateProvider %p _fireHandlersForDate: %s\n", now(), self, [[date description] UTF8String]); ((void(*)(id, SEL, id))oFire)(self, s, date); L("%.3f   fired\n", now()); }
static id hPObs(id self, SEL s, MinuteHandler h) {
	Dl_info di = {0};
	dladdr((void*)((__bridge struct BlockLayout*)h)->invoke, &di);
	const char* nm = di.dli_sname;
	L("%.3f SBFOverridableDateProvider observeMinuteUpdatesWithHandler: invoke %s\n", now(), nm ?: "?");
	MinuteHandler w = ^(id d) { L("%.3f   provider handler %s called\n", now(), nm ?: "?"); h(d); };
	id tok = ((id(*)(id, SEL, MinuteHandler))oPObs)(self, s, w);
	void** raw = (void**)(__bridge void*)self;
	id under = (__bridge id)raw[2]; id ovr = (__bridge id)raw[6];
	L("%.3f   provider %p token %s underlying %s (%p) underlyingToken %p override %s\n", now(), self, [[tok description] UTF8String],
		under ? class_getName(object_getClass(under)) : "nil", under, raw[5], ovr ? [[ovr description] UTF8String] : "nil");
	return tok;
}
static IMP swap(Class c, const char* sel, IMP h) {
	Method m = class_getInstanceMethod(c, sel_getUid(sel));
	if (!m) { L("no %s\n", sel); return NULL; }
	return method_setImplementation(m, h);
}

static void install(void) {
	static int done;
	Class c = objc_getClass("SBUIPreciseClockTimer");
	if (done || !c) return;
	done = 1;
	oTick = swap(c, "_onDisplayLink:", (IMP)hTick);
	oInterval = swap(c, "_updateDisplayLinkFrameIntervalForSecondsPastMinute:", (IMP)hInterval);
	oUpdate = swap(c, "_updateDisplayLink", (IMP)hUpdate);
	oPassed = swap(c, "_handleTimePassed", (IMP)hPassed);
	oStart = swap(c, "startMinuteUpdatesWithHandler:", (IMP)hStart);
	oObserve = swap(c, "observeMinuteUpdatesWithHandler:", (IMP)hObserve);
	Class a = objc_getClass("SBStatusBarStateAggregator");
	if (a) {
		oUpd = swap(a, "_updateTimeItemsWithDate:", (IMP)hUpd);
		oRestart = swap(a, "_restartTimeItemTimer", (IMP)hRestart);
		oStop = swap(a, "_stopTimeItemTimer", (IMP)hStop);
		oUpd0 = swap(a, "_updateTimeItems", (IMP)hUpd0);
	}
	Class pc = objc_getClass("SBFOverridableDateProvider");
	if (pc) {
		oFire = swap(pc, "_fireHandlersForDate:", (IMP)hFire);
		oPObs = swap(pc, "observeMinuteUpdatesWithHandler:", (IMP)hPObs);
	}
	L("%.3f hooks installed (aggregator %p)\n", now(), a);
	dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), 10 * NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(t, ^{
		id o = timerObj;
		id link = o ? object_getIvar(o, class_getInstanceVariable(c, "_displayLink")) : nil;
		L("%.3f poll: ticks %ld link %s paused %d\n", now(), ticks, [[link description] UTF8String],
			link ? ((BOOL(*)(id, SEL))objc_msgSend)(link, sel_getUid("isPaused")) : -1);
	});
	dispatch_resume(t);
}
static void added(const struct mach_header* mh, intptr_t slide) { install(); }

__attribute__((constructor)) static void init(void) {
	const char* want = getenv("CLKDIAG_PROGS") ?: "SpringBoard";
	if (!strstr(want, getprogname())) return;
	_dyld_register_func_for_add_image(added);
}
