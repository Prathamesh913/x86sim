// x86Sim: diagnostic shim (SpringBoard; arm64): hardware-keyboard focus. Logs how SpringBoard builds its keyboard
// focus targets (SBKeyboardFocusTarget: scene -> clientHandle -> processHandle -> pid), the coordinator's focus changes
// and the policy target it sends to backboardd. Log: X86SIM_DATA_DIR/var/log/kbdiag.<pid>.log.
// X86SIM_INSERT=<this dylib> (build: build/mac-build-diag.sh kbdiag).
#import <Foundation/Foundation.h>
#include "diagpath.h"
#include <objc/runtime.h>
#include <objc/message.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
static int logfd(void) {
	static int fd = -1;
	if (fd < 0) { char p[200]; snprintf(p, sizeof p, X86SIM_DATA_DIR "/var/log/kbdiag.%d.log", getpid()); fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644); }
	return fd;
}
static const char *D(id o) { return o ? [[[o description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "] UTF8String] : "(nil)"; }
static id call0(id o, const char *sel) {
	if (!o || ![o respondsToSelector:sel_getUid(sel)]) return nil;
	return ((id(*)(id, SEL))objc_msgSend)(o, sel_getUid(sel));
}
static void stamp(void) {
	struct timeval tv; gettimeofday(&tv, NULL);
	dprintf(logfd(), "%ld.%03d ", (long)tv.tv_sec % 1000, (int)(tv.tv_usec / 1000));
}
static IMP o_initfb, o_inittok, o_settarget, o_focuschange, o_updprev, o_defer;
static id d_initfb(id self, SEL s, id scene) {
	id ch = call0(scene, "clientHandle");
	id ph = call0(ch, "processHandle");
	int pid = ph ? ((int(*)(id, SEL))objc_msgSend)(ph, sel_getUid("pid")) : -2;
	id r = ((id(*)(id, SEL, id))o_initfb)(self, s, scene);
	stamp(); dprintf(logfd(), "_initWithFBScene %s clientHandle %s (%s) processHandle %s (%s) pid %d -> %s\n", D(call0(scene, "identifier")),
		ch ? object_getClassName(ch) : "-", D(ch), ph ? object_getClassName(ph) : "-", D(ph), pid, D(r));
	return r;
}
static id d_inittok(id self, SEL s, id tok, int pid) {
	id r = ((id(*)(id, SEL, id, int))o_inittok)(self, s, tok, pid);
	stamp(); dprintf(logfd(), "_initWithSceneIdentityToken %s pid %d -> %s\n", D(tok), pid, D(r));
	return r;
}
static void d_settarget(id self, SEL s, id t) {
	stamp(); dprintf(logfd(), "policy setKeyboardFocusTarget %s deferring %s\n", D(t), D(call0(t, "deferringTarget")));
	((void(*)(id, SEL, id))o_settarget)(self, s, t);
}
static void d_focuschange(id self, SEL s, id change, BOOL arb, id req, BOOL wait) {
	stamp(); dprintf(logfd(), "_handleFocusSceneChange %s arbiter %d request %s\n", D(change), arb, D(req));
	((void(*)(id, SEL, id, BOOL, id, BOOL))o_focuschange)(self, s, change, arb, req, wait);
}
static void d_updprev(id self, SEL s, id ident, int pid) {
	stamp(); dprintf(logfd(), "_updateKeyboardFocusTargetFromPreviouslyFocusedSceneIdentity %s pid %d\n", D(ident), pid);
	((void(*)(id, SEL, id, int))o_updprev)(self, s, ident, pid);
}
static id d_defer(id self, SEL s) {
	id r = ((id(*)(id, SEL))o_defer)(self, s);
	static int n; if (n++ < 200) { stamp(); dprintf(logfd(), "deferringTarget of %s -> %s\n", D(self), D(r)); }
	return r;
}
static void hook(const char *c, const char *sel, IMP n, IMP *o) {
	Class k = objc_getClass(c); Method m = k ? class_getInstanceMethod(k, sel_getUid(sel)) : NULL;
	if (m) *o = method_setImplementation(m, n);
	else {
		dprintf(logfd(), "no -[%s %s] (class %p)\n", c, sel, k);
		unsigned cnt = 0; Method *ms = k ? class_copyMethodList(k, &cnt) : NULL;
		for (unsigned i = 0; i < cnt; i++) dprintf(logfd(), "  has %s\n", sel_getName(method_getName(ms[i])));
		free(ms);
	}
}

// generic: every method of the listed classes goes through kb_tramp, which logs class/selector and jumps to the original
// (looked up by (class, SEL); x1 is the selector on entry)
#define NG 1024
static struct { Class k; SEL s; IMP o; int n; } g[NG];
static int ng;
IMP kb_lookup(id self, SEL s) {
	Class c = object_getClass(self);
	for (; c; c = class_getSuperclass(c))
		for (int i = 0; i < ng; i++) if (g[i].s == s && (g[i].k == c || g[i].k == object_getClass((id)c))) {
			if (g[i].n++ < 40) { stamp(); dprintf(logfd(), "%c[%s %s]\n", class_isMetaClass(g[i].k) ? '+' : '-', class_getName(g[i].k), sel_getName(s)); }
			return g[i].o;
		}
	return NULL;
}
__asm__(".text\n.globl _kb_tramp\n.p2align 2\n_kb_tramp:\n"
	"stp x29, x30, [sp, #-224]!\nmov x29, sp\n"
	"stp x0, x1, [sp, #16]\nstp x2, x3, [sp, #32]\nstp x4, x5, [sp, #48]\nstp x6, x7, [sp, #64]\nstr x8, [sp, #80]\n"
	"stp q0, q1, [sp, #96]\nstp q2, q3, [sp, #128]\nstp q4, q5, [sp, #160]\nstp q6, q7, [sp, #192]\n"
	"bl _kb_lookup\nmov x16, x0\n"
	"ldp x0, x1, [sp, #16]\nldp x2, x3, [sp, #32]\nldp x4, x5, [sp, #48]\nldp x6, x7, [sp, #64]\nldr x8, [sp, #80]\n"
	"ldp q0, q1, [sp, #96]\nldp q2, q3, [sp, #128]\nldp q4, q5, [sp, #160]\nldp q6, q7, [sp, #192]\n"
	"ldp x29, x30, [sp], #224\nbr x16\n");
extern void kb_tramp(void);
static void hookall(const char *c, int meta) {
	Class k = objc_getClass(c); if (!k) { dprintf(logfd(), "no class %s\n", c); return; }
	if (meta) k = object_getClass((id)k);
	unsigned cnt = 0; Method *ms = class_copyMethodList(k, &cnt);
	for (unsigned i = 0; i < cnt && ng < NG; i++) {
		const char *sn = sel_getName(method_getName(ms[i]));
		if (sn[0] == '.' || !strcmp(sn, "description") || !strcmp(sn, "hash") || !strcmp(sn, "isEqual:") || !strncmp(sn, "dealloc", 7)) continue;
		g[ng].k = k; g[ng].s = method_getName(ms[i]); g[ng].o = method_getImplementation(ms[i]); ng++;
		method_setImplementation(ms[i], (IMP)kb_tramp);
	}
	free(ms);
	dprintf(logfd(), "hooked %u methods of %s%s\n", cnt, meta ? "+" : "", c);
}
__attribute__((constructor)) static void init(void) {
	if (strcmp(getprogname(), "SpringBoard")) return;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		for (const char **c = (const char *[]){ "SBKeyboardFocusCoordinator", "SBKeyboardFocusSceneController", "SBMutableKeyboardFocusPolicy",
				"SBKeyboardFocusPolicy", "SBKeyboardFocusArbiter", "SBKeyboardFocusTarget", NULL }; *c; c++) { hookall(*c, 0); hookall(*c, 1); }
		dprintf(logfd(), "kbdiag hooked\n");
	});
}
