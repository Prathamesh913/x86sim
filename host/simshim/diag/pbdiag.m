// x86Sim: diagnostic shim (PosterBoard): logs PBFPosterExtensionStoreCoordinator's init arguments and its
// transient-info URL (ivar at 0x30), which was nil (NSData writeToURL: nil URL exception). Log: var/log/pbdiag.<pid>.log
#import <Foundation/Foundation.h>
#include "diagpath.h" // x86Sim: X86SIM_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <objc/runtime.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sysctl.h>
static int logfd(void)
{
	static int fd = -1;
	if (fd < 0) {
		char p[200];
		snprintf(p, sizeof(p), X86SIM_DATA_DIR "/var/log/pbdiag.%s.%d.log", getprogname(), getpid());
		fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	return fd;
}
extern NSString* PFTemporaryDirectory(void);
static id (*o_init)(id, SEL, id, id, BOOL);
static id d_init(id self, SEL s, id url, id ident, BOOL ro)
{
	id r = o_init(self, s, url, ident, ro);
	id u = r ? *(__unsafe_unretained id*) ((char*) (__bridge void*) r + 0x30) : nil;
	NSString* t = PFTemporaryDirectory();
	dprintf(logfd(), "init url=%s ident=%s ro=%d -> %p ivar30=%s tmp=%s\n", [[url description] UTF8String], [[ident description] UTF8String], ro, r, [[u description] UTF8String], [t UTF8String]);
	NSURL* tu = t ? [NSURL fileURLWithPath:t isDirectory:YES] : nil;
	dprintf(logfd(), "  tmpurl=%s append=%s\n", [[tu description] UTF8String], [[[tu URLByAppendingPathComponent:@"x-y.plist"] description] UTF8String]);
	id b = [NSString performSelector:sel_getUid("pbf_bootInstanceIdentifier")];
	id h = [ident respondsToSelector:sel_getUid("pbf_sha256Hash")] ? [ident performSelector:sel_getUid("pbf_sha256Hash")] : nil;
	char uu[128] = {0}; size_t l = sizeof(uu) - 1; int sr = sysctlbyname("kern.bootsessionuuid", uu, &l, NULL, 0);
	dprintf(logfd(), "  boot=%s hash=%s sysctl=%d '%s' len %zu\n", [[b description] UTF8String], [[h description] UTF8String], sr, uu, l);
	return r;
}
static void added(const struct mach_header* mh, intptr_t slide)
{
	if (o_init) return;
	Class c = objc_getClass("PBFPosterExtensionStoreCoordinator");
	if (!c) return;
	Method m = class_getInstanceMethod(c, sel_getUid("initWithContainerURL:extensionIdentifier:readonly:"));
	if (m) o_init = (void*) method_setImplementation(m, (IMP) d_init);
}
__attribute__((constructor)) static void init(void)
{
	if (strcmp(getprogname(), "PosterBoard")) return;
	_dyld_register_func_for_add_image(added);
}
