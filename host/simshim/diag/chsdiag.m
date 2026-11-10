// x86Sim: diagnostic shim: logs +[STRPOPACKCoder encodeObject:error:] / decodeObjectOfClass:fromData:error:
// (ChronoServices boxes such as CHSWidgetExtensionsBox are OPACK data inside NSXPC messages; SpringBoard
// crashed decoding a nil one). Log: data/var/log/chsdiag.<prog>.<pid>.log
#import <Foundation/Foundation.h>
#include "diagpath.h" // x86Sim: X86SIM_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <objc/runtime.h>
#include <fcntl.h>
#include <unistd.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
static int logfd(void)
{
	static int fd = -1;
	if (fd < 0) {
		char p[200];
		snprintf(p, sizeof(p), X86SIM_DATA_DIR "/var/log/chsdiag.%s.%d.log", getprogname(), getpid());
		fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	return fd;
}
static void bt(void)
{
	void* f[10]; int k = backtrace(f, 10);
	for (int i = 2; i < k; i++) { Dl_info di; if (dladdr(f[i], &di) && di.dli_fname) dprintf(logfd(), "    %s %s+0x%lx\n", strrchr(di.dli_fname, '/') + 1, di.dli_sname ? di.dli_sname : "", (char*) f[i] - (char*) di.dli_fbase); }
}
static id (*o_enc)(id, SEL, id, NSError**);
static id (*o_dec)(id, SEL, Class, id, NSError**);
static id d_enc(id self, SEL s, id obj, NSError** e)
{
	NSError* err = nil;
	id r = o_enc(self, s, obj, &err);
	if (e) *e = err;
	if (1) {
		dprintf(logfd(), "encode %s -> %p (%lu bytes) err %s\n", object_getClassName(obj), r, (unsigned long) [r length], [[err description] UTF8String]);
		bt();
	}
	return r;
}
static id d_dec(id self, SEL s, Class c, id data, NSError** e)
{
	NSError* err = nil;
	id r = data ? o_dec(self, s, c, data, &err) : nil;
	if (e) *e = err;
	if (1) {
		dprintf(logfd(), "decode %s from %p (%lu bytes) -> %p err %s\n", class_getName(c), data, (unsigned long) [data length], r, [[err description] UTF8String]);
		bt();
	}
	return r;
}
static void hook(void)
{
	Class m = objc_getMetaClass("STRPOPACKCoder");
	if (!m || o_enc) return;
	Method a = class_getInstanceMethod(m, sel_getUid("encodeObject:error:"));
	Method b = class_getInstanceMethod(m, sel_getUid("decodeObjectOfClass:fromData:error:"));
	if (a) o_enc = (void*) method_setImplementation(a, (IMP) d_enc);
	if (b) o_dec = (void*) method_setImplementation(b, (IMP) d_dec);
	dprintf(logfd(), "hooked %p %p\n", a, b);
}
static void added(const struct mach_header* mh, intptr_t slide) { hook(); }
__attribute__((constructor)) static void init(void)
{
	_dyld_register_func_for_add_image(added);
}
