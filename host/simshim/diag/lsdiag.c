// x86Code: diagnostic shim: logs LaunchServices' executable parsing (dyld_get_image_versions on a header copy,
// dyld_get_active_platform) in lsd, to find why every record has platform 0. Only active in lsd.
#include "diagpath.h" // x86Code: X86CODE_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
extern void dyld_get_image_versions(const void* mh, void (^cb)(uint32_t, uint32_t, uint32_t));
extern int dyld_get_active_platform(void);
#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };
static int logfd(void)
{
	static int fd = -1;
	if (fd < 0) {
		char p[200];
		snprintf(p, sizeof(p), X86CODE_DATA_DIR "/var/log/lsdiag.%s.%d.log", getprogname(), getpid());
		fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	return fd;
}
static int count;
static void d_versions(const void* mh, void (^cb)(uint32_t, uint32_t, uint32_t))
{
	__block int n = 0;
	const uint32_t* h = mh;
	dyld_get_image_versions(mh, ^(uint32_t p, uint32_t s, uint32_t m) {
		n++;
		if (0) dprintf(logfd(), "  cb platform %u sdk %x minos %x\n", p, s, m);
		cb(p, s, m);
	});
	Dl_info c; int icl = dladdr(__builtin_return_address(0), &c) && strstr(c.dli_fname, "InstalledContentLibrary");
	if (icl) { if (++count % 500 == 0) dprintf(logfd(), "%d InstalledContentLibrary calls\n", count); return; }
	{
		dprintf(logfd(), "versions mh %p magic %x cpu %x ncmds %u sizeofcmds %u -> %d callbacks, active %d\n", mh, h[0], h[1], h[4], h[5], n, dyld_get_active_platform());
		void* f[6]; int k = backtrace(f, 6);
		for (int i = 1; i < k; i++) { Dl_info di; if (dladdr(f[i], &di)) dprintf(logfd(), "    %s+0x%lx\n", strrchr(di.dli_fname, '/') + 1, (char*) f[i] - (char*) di.dli_fbase); }
	}
}
DYLD_INTERPOSE(d_versions, dyld_get_image_versions)

// log lookups of LSExecutablePlatformKey (and the caller in CoreServices)
static int pcount, islsd;
static int logfd(void);
// FSNode -getLength:error: / -getFileSystemRepresentation:error: (_LSAddExecutableFormatInfo needs both)
static BOOL (*o_len)(id, SEL, long long*, id*);
static BOOL (*o_fsr)(id, SEL, char*, id*);
static int lcount;
static void lognode(const char* what, id self, BOOL ok, long long v, id err)
{
	if (lcount++ > 3000) return;
	char p[1100] = "?"; if (o_fsr) { id e2 = nil; o_fsr(self, sel_getUid("getFileSystemRepresentation:error:"), p, &e2); }
	Dl_info di; void* ra = __builtin_return_address(1); dladdr(ra, &di);
	dprintf(logfd(), "%s %s ok %d val %lld err %p\n", what, p, ok, v, err);
}
static BOOL n_len(id self, SEL s, long long* out, id* err)
{
	BOOL ok = o_len(self, s, out, err);
	lognode("getLength", self, ok, out ? *out : -1, err ? *err : nil);
	return ok;
}
static BOOL n_fsr(id self, SEL s, char* out, id* err)
{
	BOOL ok = o_fsr(self, s, out, err);
	if (!ok) dprintf(logfd(), "getFileSystemRepresentation FAILED\n");
	return ok;
}
static id (*o_init)(id, SEL, CFURLRef, unsigned long, id*);
static int icount;
static id n_init(id self, SEL s, CFURLRef u, unsigned long fl, id* err)
{
	id r = o_init(self, s, u, fl, err);
	char p[1100] = "?"; if (u) CFURLGetFileSystemRepresentation(u, true, (UInt8*) p, sizeof p);
	if ((!r || strstr(p, "News")) && icount++ < 2000) {
		Dl_info di; void* ra = __builtin_return_address(0); dladdr(ra, &di);
		dprintf(logfd(), "FSNode init %s flags %lx -> %p (%s+0x%lx)\n", p, fl, r, strrchr(di.dli_fname, '/') + 1, (char*) ra - (char*) di.dli_fbase);
	}
	return r;
}
static void swizzle(void)
{
	Class c = objc_getClass("FSNode");
	if (!c) { dprintf(logfd(), "no FSNode\n"); return; }
	Method m = class_getInstanceMethod(c, sel_getUid("getLength:error:"));
	o_len = (void*) method_setImplementation(m, (IMP) n_len);
	m = class_getInstanceMethod(c, sel_getUid("getFileSystemRepresentation:error:"));
	o_fsr = (void*) method_setImplementation(m, (IMP) n_fsr);
	m = class_getInstanceMethod(c, sel_getUid("initWithURL:flags:error:"));
	o_init = (void*) method_setImplementation(m, (IMP) n_init);
}
static const void* key; // CoreServices' CFSTR("LSExecutablePlatformKey") (26.5 x86_64 slice offsets)
__attribute__((constructor)) static void init(void)
{
	islsd = strcmp(getprogname(), "lsd") == 0;
	if (!islsd) return;
	void* h = dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
	char* anchor = dlsym(h, "CSIdentityAuthorityGetTypeID");
	if (anchor) key = anchor - 0x1ba41a + 0x285878;
	swizzle();
}
static const void* d_get(CFDictionaryRef d, const void* k)
{
	const void* v = CFDictionaryGetValue(d, k);
	if (!islsd || !key || k != key) return v;
	if (pcount++ < 400) {
		int val = -1; if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberIntType, &val);
		Dl_info di; void* ra = __builtin_return_address(0); dladdr(ra, &di);
		CFTypeRef bid = CFDictionaryGetValue(d, CFSTR("CFBundleIdentifier"));
		char b[200] = "?"; if (bid && CFGetTypeID(bid) == CFStringGetTypeID()) CFStringGetCString(bid, b, sizeof b, kCFStringEncodingUTF8);
		dprintf(logfd(), "getPlatformKey %s+0x%lx dict %p (%ld keys, %s) -> %p (%d)\n", strrchr(di.dli_fname, '/') + 1, (char*) ra - (char*) di.dli_fbase, d, CFDictionaryGetCount(d), b, v, val);
	}
	return v;
}
DYLD_INTERPOSE(d_get, CFDictionaryGetValue)
// log opens of bundle executables (paths inside .app/.appex that are not resources)
#include <errno.h>
#include <stdarg.h>
static int ocount;
static int d_open(const char* path, int flags, ...)
{
	va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
	int r = open(path, flags, mode);
	int e = errno;
	if (islsd && path && (strstr(path, ".app") ) && !strstr(path, ".plist") && !strstr(path, ".lproj") && ocount++ < 3000) {
		Dl_info di; void* ra = __builtin_return_address(0); dladdr(ra, &di);
		dprintf(logfd(), "open %s flags %x -> %d errno %d (%s+0x%lx)\n", path, flags, r, r < 0 ? e : 0, strrchr(di.dli_fname, '/') + 1, (char*) ra - (char*) di.dli_fbase);
	}
	errno = e;
	return r;
}
DYLD_INTERPOSE(d_open, open)
