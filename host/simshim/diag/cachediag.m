// x86Sim: diagnostic shim (arm64): does dyld_sim find a framework in the shared cache by the paths CFBundle uses?
// In processes named in X86SIM_CACHEDIAG (default com.apple.WebKit.WebContent) logs _dyld_shared_cache_contains_path
// and realpath for WebKit's executable path forms, and what CFBundle reports for WebKit.framework.
// Log: X86SIM_DATA_DIR/var/log/cachediag.<pid>.log. X86SIM_INSERT=<this dylib> (build/mac-build-diag.sh).
#import <Foundation/Foundation.h>
#include "diagpath.h"
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern bool _dyld_shared_cache_contains_path(const char *path);
__attribute__((constructor)) static void init(void) {
	const char *want = getenv("X86SIM_CACHEDIAG") ?: "com.apple.WebKit.WebContent";
	if (!strstr(want, getprogname())) return;
	char p[300]; snprintf(p, sizeof p, X86SIM_DATA_DIR "/var/log/cachediag.%d.log", getpid());
	int fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	const char *root = getenv("DYLD_ROOT_PATH") ?: "";
	char a[1024], b[1024], c[1024];
	snprintf(a, sizeof a, "%s/System/Library/Frameworks/WebKit.framework/WebKit", root);
	snprintf(b, sizeof b, "%s/System/Cryptexes/OS/System/Library/Frameworks/WebKit.framework/WebKit", root);
	char rr[PATH_MAX] = "", d[1024], e[1024];
	realpath(root, rr);
	snprintf(d, sizeof d, "%s/System/Library/Frameworks/WebKit.framework/WebKit", rr);
	snprintf(e, sizeof e, "%s/System/Cryptexes/OS/System/Library/Frameworks/WebKit.framework/WebKit", rr);
	const char *paths[] = { a, b, d, e, "/System/Library/Frameworks/WebKit.framework/WebKit",
		"/System/Cryptexes/OS/System/Library/Frameworks/WebKit.framework/WebKit", NULL };
	dprintf(fd, "DYLD_ROOT_PATH=%s\n", root);
	for (const char **q = paths; *q; q++) dprintf(fd, "contains %d %s\n", _dyld_shared_cache_contains_path(*q), *q);
	snprintf(c, sizeof c, "%s/System/Library/Frameworks/WebKit.framework", root);
	char r[PATH_MAX]; dprintf(fd, "realpath %s -> %s\n", c, realpath(c, r) ? r : "(fail)");
	@autoreleasepool {
		NSBundle *wk = [NSBundle bundleWithPath:[NSString stringWithUTF8String:c]];
		dprintf(fd, "NSBundle %s executablePath %s\n", wk.bundlePath.UTF8String, wk.executablePath.UTF8String ?: "(nil)");
	}
	close(fd);
}
