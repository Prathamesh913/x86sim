// x86Code: diagnostic shim (runningboardd): which LSBundleProxy does RunningBoard get for extension processes,
// and what extension point identifier does it report ("could not find extension point for xpcservice<...>")
#import <Foundation/Foundation.h>
#include "diagpath.h" // x86Code: X86CODE_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <dlfcn.h>
#include <objc/runtime.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <unistd.h>
static int logfd(void) {
	static int fd = -1;
	if (fd < 0) { char p[200]; snprintf(p, sizeof p, X86CODE_DATA_DIR "/var/log/rbdiag.%d.log", getpid()); fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644); }
	return fd;
}
static IMP o_ep;
static id d_ep(id self, SEL s) {
	id r = ((id(*)(id, SEL))o_ep)(self, s);
	static int n; if (n++ < 60) dprintf(logfd(), "_rb_extensionPointIdentifier %s %s -> %s\n", object_getClassName(self), [[[self performSelector:sel_getUid("bundleIdentifier")] description] UTF8String], [[r description] UTF8String]);
	return r;
}
static IMP o_x1, o_x2;
static id d_x(id self, SEL s) {
	IMP o = [self isKindOfClass:objc_getClass("RBLSBundleProperties")] ? o_x1 : o_x2;
	id r = ((id(*)(id, SEL))o)(self, s);
	static int n; if (n++ < 80) dprintf(logfd(), "-[%s extensionPointIdentifier] %s path %s -> %s\n", object_getClassName(self), [[[self performSelector:sel_getUid("bundleIdentifier")] description] UTF8String], [[[self performSelector:sel_getUid("bundlePath")] description] UTF8String], [[r description] UTF8String]);
	return r;
}
static IMP o_pfi;
static id d_pfi(id self, SEL s, id ident) {
	id r = ((id(*)(id, SEL, id))o_pfi)(self, s, ident);
	static int n; if (n++ < 40) dprintf(logfd(), "propertiesForIdentity %s -> %s %s\n", [[ident description] UTF8String], object_getClassName(r), [[[r performSelector:sel_getUid("bundleIdentifier")] description] UTF8String]);
	return r;
}
#define CM(name) static IMP o_##name; static id d_##name(id cls, SEL s, id a) { id r = ((id(*)(id, SEL, id))o_##name)(cls, s, a); \
	static int n; if (n++ < 60) dprintf(logfd(), "+[%s %s] %s -> %s\n", class_getName(cls), sel_getName(s), [[a description] UTF8String], [[r description] UTF8String]); return r; }
CM(c1) CM(c2) CM(c3) CM(c4)
static void hc(const char* c, const char* sel, IMP n, IMP* o) { Class k = objc_getClass(c); Method m = k ? class_getClassMethod(k, sel_getUid(sel)) : NULL; if (m) *o = method_setImplementation(m, n); else dprintf(logfd(), "no +[%s %s]\n", c, sel); }
#define IM0(name) static IMP o_##name; static id d_##name(id self, SEL s) { id r = ((id(*)(id, SEL))o_##name)(self, s); \
	static int n; if (n++ < 60) dprintf(logfd(), "-[%s %s] -> %s %s\n", object_getClassName(self), sel_getName(s), object_getClassName(r), [[r description] UTF8String]); return r; }
IM0(lsb) IM0(xpb)
static IMP o_bsx; static id d_bsx(id self, SEL s, id a) { id r = ((id(*)(id, SEL, id))o_bsx)(self, s, a);
	static int n; if (n++ < 60) { NSString* (*ep)(int) = dlsym(RTLD_DEFAULT, "RBSExecutablePathForPID");
		dprintf(logfd(), "BSXPC propertiesForIdentifier %s -> %s bid %s exe %s ep %s; now exe %s\n", [[a description] UTF8String], object_getClassName(r), [[[r performSelector:sel_getUid("bundleIdentifier")] description] UTF8String], [[[r performSelector:sel_getUid("executablePath")] description] UTF8String], [[[r performSelector:sel_getUid("extensionPointIdentifier")] description] UTF8String], [ep([[a description] intValue]) UTF8String]); }
	return r; }
static IMP o_init;
static id d_init(id self, SEL s, id proxy) {
	id r = ((id(*)(id, SEL, id))o_init)(self, s, proxy);
	id ep = [proxy respondsToSelector:sel_getUid("_rb_extensionPointIdentifier")] ? [proxy performSelector:sel_getUid("_rb_extensionPointIdentifier")] : nil;
	id bid = [proxy respondsToSelector:sel_getUid("bundleIdentifier")] ? [proxy performSelector:sel_getUid("bundleIdentifier")] : nil;
	dprintf(logfd(), "initWithBundleProxy: %s %s ep=%s\n", object_getClassName(proxy), [[bid description] UTF8String], [[ep description] UTF8String]);
	return r;
}
#include <dlfcn.h>
static void added(const struct mach_header* mh, intptr_t slide) {
	{ Dl_info di; if (dladdr(mh, &di) && strstr(di.dli_fname, "RunningBoard.framework/RunningBoard")) {
		unsigned long refs[] = { 0xdd9e0, 0xdd9e8, 0xdd9f0, 0xdd1b8, 0xdd228 };
		for (int i = 0; i < 5; i++) dprintf(logfd(), "selref %lx = %s\n", refs[i], sel_getName(*(SEL*)((char*)mh + refs[i])));
	} }
	Class p = objc_getClass("LSBundleProxy");
	if (p && !o_ep) { Method m = class_getInstanceMethod(p, sel_getUid("_rb_extensionPointIdentifier")); if (m) { o_ep = method_setImplementation(m, (IMP)d_ep); dprintf(logfd(), "hooked ep\n"); } }
	Class c = nil;
	if (!o_pfi && objc_getClass("RBBundlePropertiesLSProvider")) {
		o_pfi = method_setImplementation(class_getInstanceMethod(objc_getClass("RBBundlePropertiesLSProvider"), sel_getUid("propertiesForIdentity:")), (IMP)d_pfi);
		hc("LSPlugInKitProxy", "pluginKitProxyForIdentifier:", (IMP)d_c1, &o_c1);
		o_lsb = method_setImplementation(class_getInstanceMethod(objc_getClass("RBBundleProperties"), sel_getUid("_lsBundleProperties")), (IMP)d_lsb);
		o_xpb = method_setImplementation(class_getInstanceMethod(objc_getClass("RBBundleProperties"), sel_getUid("_xpcBundleProperties")), (IMP)d_xpb);
		o_bsx = method_setImplementation(class_getInstanceMethod(objc_getClass("RBBundlePropertiesBSXPCProvider"), sel_getUid("propertiesForIdentifier:")), (IMP)d_bsx);
		hc("LSApplicationProxy", "applicationProxyForIdentifier:", (IMP)d_c2, &o_c2);
		hc("LSBundleProxy", "bundleProxyForIdentifier:", (IMP)d_c3, &o_c3);
		hc("LSPlugInKitProxy", "pluginKitProxyForURL:", (IMP)d_c4, &o_c4);
	}
	if (!o_x1 && objc_getClass("RBLSBundleProperties")) {
		o_x1 = method_setImplementation(class_getInstanceMethod(objc_getClass("RBLSBundleProperties"), sel_getUid("extensionPointIdentifier")), (IMP)d_x);
		Method m2 = class_getInstanceMethod(objc_getClass("RBBundleProperties"), sel_getUid("extensionPointIdentifier"));
		if (m2) o_x2 = method_setImplementation(m2, (IMP)d_x);
	}
	if (c && !o_init) { Method m = class_getInstanceMethod(c, sel_getUid("initWithBundleProxy:")); if (m) o_init = method_setImplementation(m, (IMP)d_init); dprintf(logfd(), "hooked %p\n", m); }
}
__attribute__((constructor)) static void init(void) { if (strcmp(getprogname(), "runningboardd")) return; _dyld_register_func_for_add_image(added); }
