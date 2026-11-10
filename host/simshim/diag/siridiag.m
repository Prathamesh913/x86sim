// x86Sim: diagnostic shim (SpringBoard, runningboardd; arm64): why SpringBoard has no FBApplicationProcess for
// osservice<com.apple.campo> (Siri): logs FrontBoard's process-class decision, RunningBoard's angel answer and the
// scene transaction's client process. Log: X86SIM_DATA_DIR/var/log/siridiag.<pid>.log. X86SIM_INSERT=<this dylib>.
#import <Foundation/Foundation.h>
#include "diagpath.h"
#include <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <unistd.h>
static int logfd(void) {
	static int fd = -1;
	if (fd < 0) { char p[200]; snprintf(p, sizeof p, X86SIM_DATA_DIR "/var/log/siridiag.%d.log", getpid()); fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644); }
	return fd;
}
static const char *D(id o) { return o ? [[o description] UTF8String] : "(nil)"; }
static IMP o_angel, o_cls, o_notify, o_treat, o_srvangel;
static BOOL d_angel(id self, SEL s, id ident, NSError **err) {
	BOOL r = ((BOOL(*)(id, SEL, id, NSError **))o_angel)(self, s, ident, err);
	dprintf(logfd(), "isIdentityAnAngel %s -> %d err %s\n", D(ident), r, err ? D(*err) : "-");
	return r;
}
static BOOL d_treat(id self, SEL s, NSError **err) {
	BOOL r = ((BOOL(*)(id, SEL, NSError **))o_treat)(self, s, err);
	dprintf(logfd(), "treatedAsAnAppByFrontBoard %s (%s) -> %d err %s\n", D(self), object_getClassName(self), r, err ? D(*err) : "-");
	return r;
}
static Class d_cls(id self, SEL s) {
	Class r = ((Class(*)(id, SEL))o_cls)(self, s);
	dprintf(logfd(), "fb_processClass %s -> %s\n", D(self), class_getName(r));
	return r;
}
static void d_notify(id self, SEL s, id client) {
	id lp = [client respondsToSelector:sel_getUid("legacyProcess")] ? [client performSelector:sel_getUid("legacyProcess")] : nil;
	dprintf(logfd(), "_notifyObserversOfClientConnection client %s (%s) legacyProcess %s (%s)\n", D(client), object_getClassName(client), D(lp), lp ? object_getClassName(lp) : "-");
	((void(*)(id, SEL, id))o_notify)(self, s, client);
}
static id d_srvangel(id self, SEL s, id a, id b) {
	id r = ((id(*)(id, SEL, id, id))o_srvangel)(self, s, a, b);
	dprintf(logfd(), "server %s %s %s -> %s\n", sel_getName(s), D(a), D(b), D(r));
	return r;
}
static int hook(const char *c, const char *sel, IMP n, IMP *o) {
	Class k = objc_getClass(c); Method m = k ? class_getInstanceMethod(k, sel_getUid(sel)) : NULL;
	if (!m) return 0;
	*o = method_setImplementation(m, n); dprintf(logfd(), "hooked -[%s %s]\n", c, sel); return 1;
}
static void try_hooks(int left) {
	static int h1, h2, h3, h4;
	if (!h1) h1 = hook("RBSConnection", "isIdentityAnAngel:withError:", (IMP)d_angel, &o_angel);
	if (!h2) h2 = hook("RBSProcessIdentity", "fb_processClass", (IMP)d_cls, &o_cls);
	if (!h3) h3 = hook("SBApplicationSceneUpdateTransaction", "_notifyObserversOfClientConnection:", (IMP)d_notify, &o_notify);
	if (!h4) h4 = hook("RBSOSServiceProcessIdentity", "treatedAsAnAppByFrontBoard:", (IMP)d_treat, &o_treat);
	if (!(h1 && h2 && h3 && h4) && left > 0)
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{ try_hooks(left - 1); });
}
static void server_hooks(void) {
	// runningboardd's answer: -[RBConnectionClient isIdentityAnAngel:...] style handlers; log every method whose name has "ngel"
	unsigned n; Class *cl = objc_copyClassList(&n);
	for (unsigned i = 0; i < n; i++) {
		const char *cn = class_getName(cl[i]); if (strncmp(cn, "RB", 2)) continue;
		unsigned mc; Method *ms = class_copyMethodList(cl[i], &mc);
		for (unsigned j = 0; j < mc; j++) {
			const char *sn = sel_getName(method_getName(ms[j]));
			if (strstr(sn, "ngel")) dprintf(logfd(), "server method -[%s %s] %s\n", cn, sn, method_getTypeEncoding(ms[j]));
		}
		free(ms);
	}
	free(cl);
}
__attribute__((constructor)) static void init(void) {
	const char *p = getprogname();
	if (!strcmp(p, "SpringBoard")) try_hooks(60);
	else if (!strcmp(p, "runningboardd"))
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{ server_hooks(); });
}
