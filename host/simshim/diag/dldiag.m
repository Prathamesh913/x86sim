// x86Code: diagnostic shim (Speed 30, arm64): who keeps a CADisplayLink running. Swizzles CADisplayLink's
// +displayLinkWithTarget:selector:, -setPaused:, -addToRunLoop:forMode:, -invalidate, -setPreferredFrameRateRange: and
// logs the target's class, the selector and the caller's return addresses (raw, unslid: `ipsw dyld a2s`).
// Every 10 s it lists the live links (paused or not). Log: /private/var/tmp/x86code-a64/dldiag.<prog>.<pid>.log
// Enabled by DLDIAG=1 in the process's environment (e.g. libarm64jit's a64-penv file).
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <objc/runtime.h>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

static int lfd = -1;
static NSHashTable* links;
static dispatch_source_t g_timer;
static void lg(NSString* s)
{
	if (lfd < 0) return;
	const char* c = [[NSString stringWithFormat:@"%.3f %@\n", CFAbsoluteTimeGetCurrent(), s] UTF8String];
	write(lfd, c, strlen(c));
}
static NSString* bt(void)
{
	void* f[10]; int k = backtrace(f, 10);
	NSMutableString* s = [NSMutableString string];
	for (int i = 2; i < k; i++) [s appendFormat:@" %lx", (unsigned long) f[i]];
	return s;
}
static NSString* desc(CADisplayLink* l)
{
	id t = nil; SEL sel = NULL;
	@try { t = [l valueForKey:@"target"]; } @catch (id e) {}
	Ivar iv = class_getInstanceVariable([l class], "_impl");
	(void) iv;
	CAFrameRateRange r = l.preferredFrameRateRange;
	return [NSString stringWithFormat:@"link %p target %@ paused %d fps %ld range %.0f-%.0f/%.0f", l, t ? NSStringFromClass([t class]) : @"?", (int) l.paused, (long) l.preferredFramesPerSecond, r.minimum, r.maximum, r.preferred];
	(void) sel;
}
static IMP o_make, o_paused, o_add, o_inval, o_start;
static NSString* ivars(id o)
{
	NSMutableString* s = [NSMutableString string];
	for (Class c = [o class]; c && c != [NSObject class]; c = class_getSuperclass(c)) {
		unsigned n = 0; Ivar* v = class_copyIvarList(c, &n);
		for (unsigned i = 0; i < n; i++) {
			const char* t = ivar_getTypeEncoding(v[i]);
			ptrdiff_t off = ivar_getOffset(v[i]);
			if (t[0] == '@') {
				id x = object_getIvar(o, v[i]);
				[s appendFormat:@"\n      %s = %@", ivar_getName(v[i]), x ? [[x description] substringToIndex:MIN((NSUInteger) 300, [[x description] length])] : @"nil"];
			} else
				[s appendFormat:@"\n      %s (%s) = %#llx", ivar_getName(v[i]), t, *(unsigned long long*) ((char*) (__bridge void*) o + off) & (t[0] == 'B' || t[0] == 'c' ? 0xff : ~0ull)];
		}
		free(v);
	}
	return s;
}
static void my_start(id self, SEL _cmd)
{
	lg([NSString stringWithFormat:@"_UIMotionEffectEngine _startGeneratingUpdates %p from%@ ivars:%@", self, bt(), ivars(self)]);
	((void (*)(id, SEL)) o_start)(self, _cmd);
}
static id my_make(id cls, SEL _cmd, id target, SEL sel)
{
	CADisplayLink* l = ((id (*)(id, SEL, id, SEL)) o_make)(cls, _cmd, target, sel);
	@synchronized(links) { [links addObject:l]; }
	lg([NSString stringWithFormat:@"create %p target %@ sel %@ from%@", l, NSStringFromClass([target class]), NSStringFromSelector(sel), bt()]);
	return l;
}
static void my_paused(CADisplayLink* self, SEL _cmd, BOOL p)
{
	if (self.paused != p) lg([NSString stringWithFormat:@"setPaused:%d %@ from%@", p, desc(self), bt()]);
	((void (*)(id, SEL, BOOL)) o_paused)(self, _cmd, p);
}
static void my_add(CADisplayLink* self, SEL _cmd, NSRunLoop* rl, NSRunLoopMode m)
{
	lg([NSString stringWithFormat:@"addToRunLoop %@ mode %@ from%@", desc(self), m, bt()]);
	((void (*)(id, SEL, id, id)) o_add)(self, _cmd, rl, m);
}
static void my_inval(CADisplayLink* self, SEL _cmd)
{
	lg([NSString stringWithFormat:@"invalidate %@", desc(self)]);
	@synchronized(links) { [links removeObject:self]; }
	((void (*)(id, SEL)) o_inval)(self, _cmd);
}
__attribute__((constructor)) static void dldiag_init(void)
{
	if (!getenv("DLDIAG")) return;
	char p[512];
	snprintf(p, sizeof(p), "/private/var/tmp/x86code-a64/dldiag.%s.%d.log", getprogname(), getpid());
	lfd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	links = [NSHashTable weakObjectsHashTable];
	Class c = objc_getClass("CADisplayLink");
	Method m = class_getClassMethod(c, @selector(displayLinkWithTarget:selector:));
	o_make = method_setImplementation(m, (IMP) my_make);
	o_paused = method_setImplementation(class_getInstanceMethod(c, @selector(setPaused:)), (IMP) my_paused);
	o_add = method_setImplementation(class_getInstanceMethod(c, @selector(addToRunLoop:forMode:)), (IMP) my_add);
	o_inval = method_setImplementation(class_getInstanceMethod(c, @selector(invalidate)), (IMP) my_inval);
	Class me = objc_getClass("_UIMotionEffectEngine");
	if (me) o_start = method_setImplementation(class_getInstanceMethod(me, sel_getUid("_startGeneratingUpdates")), (IMP) my_start);
	lg(@"dldiag loaded");
	dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), 10 * NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(t, ^{
		NSArray* a;
		@synchronized(links) { a = links.allObjects; }
		NSMutableString* s = [NSMutableString stringWithFormat:@"live links: %lu", (unsigned long) a.count];
		for (CADisplayLink* l in a) [s appendFormat:@"\n    %@", desc(l)];
		lg(s);
	});
	g_timer = t;
	dispatch_resume(t);
}
