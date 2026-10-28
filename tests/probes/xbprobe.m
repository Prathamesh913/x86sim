// x86Code: xpc_bundle_create / RBSExecutablePathForPID / RBSBundleIDForPID as RunningBoard's RBXPCBundleProperties
// uses them for extension processes ("could not find extension point for xpcservice<...>")
#import <Foundation/Foundation.h>
#include "probe_paths.h" // x86Code: X86CODE_RUNTIME_ROOT
#include <xpc/xpc.h>
#include <unistd.h>
#include <stdlib.h>
typedef struct _xpc_bundle_s* xpc_bundle_t;
extern xpc_object_t xpc_bundle_create(const char* path, unsigned int flags);
extern const char* xpc_bundle_get_executable_path(xpc_object_t b);
extern xpc_object_t xpc_bundle_get_info_dictionary(xpc_object_t b);
extern xpc_object_t xpc_bundle_get_property(xpc_object_t b, unsigned int p);
#include <dlfcn.h>
int proc_listallpids(void*, int);
static NSString* (*RBSExecutablePathForPID)(pid_t);
static NSString* (*RBSBundleIDForPID)(pid_t);
int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	void* h = dlopen("/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServices", RTLD_NOW);
	RBSExecutablePathForPID = dlsym(h, "RBSExecutablePathForPID"); RBSBundleIDForPID = dlsym(h, "RBSBundleIDForPID");
	printf("dl %p %p %p\n", h, RBSExecutablePathForPID, RBSBundleIDForPID);
	const char* p = argc > 1 ? argv[1] : X86CODE_RUNTIME_ROOT "/System/Library/ExtensionKit/Extensions/MercuryPosterExtension.appex";
	for (unsigned f = 0; f < 1; f++) {
		xpc_object_t b = xpc_bundle_create(p, f);
		printf("xpc_bundle_create(flags %u) = %p\n", f, b);
		if (!b) continue;
		printf("  exec %s\n", xpc_bundle_get_executable_path(b));
		xpc_object_t info = xpc_bundle_get_info_dictionary(b);
		printf("  info %p CFBundleIdentifier %s\n", info, info ? xpc_dictionary_get_string(info, "CFBundleIdentifier") : "-");
		for (unsigned k = 0; k < 0; k++) { xpc_object_t v = xpc_bundle_get_property(b, k); if (v) { char* d = xpc_copy_description(v); printf("  prop %u: %.200s\n", k, d); free(d); } }
	}
	printf("self exe %s bundle %s\n", [RBSExecutablePathForPID(getpid()) UTF8String], [RBSBundleIDForPID(getpid()) UTF8String]);
	sleep(40);
	int pids[2048]; int n = proc_listallpids(pids, sizeof pids);
	printf("%d pids\n", n);
	for (int i = 0; i < n; i++) { NSString* e = RBSExecutablePathForPID(pids[i]); if (!e || [e containsString:@".appex"] || [e containsString:@"Poster"]) printf("pid %d exe %s bundle %s\n", pids[i], [e UTF8String], [RBSBundleIDForPID(pids[i]) UTF8String]); }
	return 0;
}
