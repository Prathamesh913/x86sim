// x86Code: diagnostic shim (cloudd): what -[CKDXPCConnection getContainerScopedDaemonProxyCreatorForSetupInfo:
// containerScopedClientProxy:completionHandler:] answers (CloudKit clients log "Ignoring failed attempt to get container
// proxy ... (null)" and then trap on accountInfo (nil, nil)). Log: data/var/log/ckdiag.<pid>.log
// X86CODE_INSERT=host/simshim/diag/libckdiag.dylib
#import <Foundation/Foundation.h>
#include "diagpath.h" // x86Code: X86CODE_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <objc/runtime.h>
#include <objc/message.h>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
static int logfd(void) {
	static int fd = -1;
	if (fd < 0) { char p[200]; snprintf(p, sizeof p, X86CODE_DATA_DIR "/var/log/ckdiag.%d.log", getpid()); fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644); }
	return fd;
}
static void bt(void) { void* f[24]; int n = backtrace(f, 24); backtrace_symbols_fd(f, n, logfd()); }
static IMP o_get;
static void d_get(id self, SEL s, id setup, id clientProxy, void (^completion)(id, id)) {
	dprintf(logfd(), "getContainerScoped... self=%s setup=%s proxy=%s\n", object_getClassName(self), [[setup description] UTF8String], [[clientProxy description] UTF8String]);
	void (^wrapped)(id, id) = ^(id creator, id error) {
		dprintf(logfd(), " -> creator=%s error=%s\n", [[creator description] UTF8String], [[error description] UTF8String]);
		if (!creator && !error) bt();
		completion(creator, error);
	};
	((void (*)(id, SEL, id, id, id))o_get)(self, s, setup, clientProxy, wrapped);
}
static void* hook(void* arg) {
	for (int i = 0; i < 200; i++) {
		Class c = objc_getClass("CKDXPCConnection");
		if (c) {
			SEL sel = sel_getUid("getContainerScopedDaemonProxyCreatorForSetupInfo:containerScopedClientProxy:completionHandler:");
			Method m = class_getInstanceMethod(c, sel);
			if (m) { o_get = method_setImplementation(m, (IMP)d_get); dprintf(logfd(), "hooked\n"); }
			else dprintf(logfd(), "no method\n");
			// where processIsAttached comes from
			Class p = objc_getClass("CKDProcessScopedClientProxy");
			for (Class k = p; k; k = class_getSuperclass(k)) {
				unsigned n; Method* ml = class_copyMethodList(k, &n);
				for (unsigned i = 0; i < n; i++) {
					Dl_info di; IMP imp = method_getImplementation(ml[i]);
					dladdr((void*)imp, &di);
					dprintf(logfd(), "%s -%s imp %s+0x%lx\n", class_getName(k), sel_getName(method_getName(ml[i])),
						di.dli_fname ? strrchr(di.dli_fname, '/') + 1 : "?", (unsigned long)((char*)imp - (char*)di.dli_fbase));
				}
				free(ml);
				unsigned ni; Ivar* iv = class_copyIvarList(k, &ni);
				for (unsigned i = 0; i < ni; i++) dprintf(logfd(), "%s ivar %s @%ld\n", class_getName(k), ivar_getName(iv[i]), (long)ivar_getOffset(iv[i]));
				free(iv);
				if (!strcmp(class_getName(k), "NSObject")) break;
			}
			return NULL;
		}
		usleep(50000);
	}
	return NULL;
}
__attribute__((constructor)) static void init(void) {
	if (strcmp(getprogname(), "cloudd")) return;
	pthread_t t; pthread_create(&t, NULL, hook, NULL); pthread_detach(t);
}
