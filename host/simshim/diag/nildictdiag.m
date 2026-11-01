// x86Code: diagnostic shim (arm64): which dictionary gets a nil object. In processes named in X86CODE_NILDICT (default
// Spotlight) +[NSDictionary dictionaryWithObjects:forKeys:count:] logs keys, the non-nil objects and the caller when an
// object is nil (CoreSpotlight's _CSSuggestionToken threw "attempt to insert nil object from objects[0]").
// Log: X86CODE_DATA_DIR/var/log/nildict.<pid>.log. X86CODE_INSERT=<this dylib> (DIAG_ARCH=arm64 build/mac-build-diag.sh).
#import <Foundation/Foundation.h>
#include "diagpath.h"
#include <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
static IMP o_dwo;
static int logfd(void) {
	static int fd = -1;
	if (fd < 0) { char p[200]; snprintf(p, sizeof p, X86CODE_DATA_DIR "/var/log/nildict.%d.log", getpid()); fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644); }
	return fd;
}
static id d_dwo(id cls, SEL s, const id *objs, const id *keys, NSUInteger n) {
	for (NSUInteger i = 0; i < n; i++) if (!objs[i]) {
		Dl_info di = {0}; void *ra = __builtin_return_address(0); dladdr(ra, &di);
		dprintf(logfd(), "nil object %lu of %lu, caller %p %s+%#lx\n", (unsigned long)i, (unsigned long)n, ra,
			di.dli_sname ?: "?", (unsigned long)((char *)ra - (char *)di.dli_saddr));
		for (NSUInteger k = 0; k < n; k++)
			dprintf(logfd(), "  [%lu] key %s = %s (%s)\n", (unsigned long)k, [[keys[k] description] UTF8String] ?: "(nil)",
				objs[k] ? [[objs[k] description] UTF8String] : "(nil)", objs[k] ? object_getClassName(objs[k]) : "-");
		break;
	}
	return ((id(*)(id, SEL, const id *, const id *, NSUInteger))o_dwo)(cls, s, objs, keys, n);
}
__attribute__((constructor)) static void init(void) {
	const char *want = getenv("X86CODE_NILDICT") ?: "Spotlight";
	if (strcmp(want, getprogname())) return;
	Method m = class_getClassMethod(objc_getClass("NSDictionary"), @selector(dictionaryWithObjects:forKeys:count:));
	if (m) o_dwo = method_setImplementation(m, (IMP)d_dwo);
}
