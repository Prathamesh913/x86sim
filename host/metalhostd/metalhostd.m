// x86Code: metalhostd -- the Metal simulator service of our iOS-simulator device (Darling side).
//
// Does what MTLSimDriverHost.xpc / CoreSimulator's SimMetalHost do on a Mac: loads the runtime's own
// MTLSimImplementation and hands it a listener (init_with_xpc_connection). MTLSimImplementation then serves
// the apps' MTLSimDriver connections (handshake, buffers, serialized object commands and command buffers)
// and replays them on MTLCreateSystemDefaultDevice() -- which in this process is libvkmtl's VKMTLDevice
// (vkmtl.m), forwarding to mtlvk on Vulkan.
//
// Service names: the MTLSimDriver client looks up "com.apple.metal.simulator.<progname>" first and then
// "com.apple.metal.simulator". The launchd job (cli/libexec/mksimdevice.py, X86CODE_METAL) lists the names it
// owns as MachServices; X86CODE_METAL_NAMES (comma-separated) tells us which ones to listen on. Keeping the
// generic name unregistered keeps system processes (backboardd, SpringBoard: CoreAnimation) on their
// software renderer while only opted-in processes get a device.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <xpc/xpc.h>
#import <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "vkmtl_proto.h"

// marked unavailable on iOS in the SDK, but libxpc in the simulator runtime has it (MTLSimDriverHost's peer
// listener API needs the XPC-service bootstrap we don't have)
extern xpc_connection_t x86_xpc_connection_create_mach_service(const char* name, dispatch_queue_t q, uint64_t flags)
	__asm__("_xpc_connection_create_mach_service");

extern id<MTLDevice> VKMTLCreateSystemDefaultDevice(void);
extern NSArray* VKMTLCopyAllDevices(void);
extern int VKMTLRebind(const char* image_name_part, void* real, void* ours);

#define RT_MTLSIMIMPL "/System/Library/PrivateFrameworks/MTLSimImplementation.framework/MTLSimImplementation"

int main(int argc, char** argv) {
	(void)argc; (void)argv;
	setvbuf(stderr, NULL, _IOLBF, 0);
	@autoreleasepool {
		void* impl = dlopen(RT_MTLSIMIMPL, RTLD_NOW);
		if (!impl) { fprintf(stderr, "metalhostd: dlopen MTLSimImplementation: %s\n", dlerror()); return 1; }
		void (*init_with_xpc_connection)(xpc_connection_t, uint64_t, uint64_t) = dlsym(impl, "init_with_xpc_connection");
		if (!init_with_xpc_connection) { fprintf(stderr, "metalhostd: no init_with_xpc_connection\n"); return 1; }
		// MTLSimImplementation (and MetalSerializer) get their device from MTLCreateSystemDefaultDevice: ours
		int n = 0;
		void* real_create = dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice");
		void* real_all = dlsym(RTLD_DEFAULT, "MTLCopyAllDevices");
		const char* images[] = { "MTLSimImplementation", "MetalSerializer", NULL };
		for (int i = 0; images[i]; i++) {
			n += VKMTLRebind(images[i], real_create, (void*)VKMTLCreateSystemDefaultDevice);
			n += VKMTLRebind(images[i], real_all, (void*)VKMTLCopyAllDevices);
		}
		fprintf(stderr, "metalhostd: %d device entry points rebound\n", n);

		id<MTLDevice> dev = VKMTLCreateSystemDefaultDevice();
		if (!dev) {
			// x86Code: no mtlvk (not started, or it died): keep the service names but cancel every client connection
			// at once, so MTLSimDriver reports no device and CoreAnimation/apps use their software paths instead of
			// waiting on a service that never answers. mtlvk is looked for again every 10 s; when it is back we exit
			// and launchd starts a serving metalhostd on the next lookup (processes already running stay software).
			fprintf(stderr, "metalhostd: no device (is mtlvk running?): refusing clients\n");
			const char* names = getenv("X86CODE_METAL_NAMES");
			for (NSString* name in [@(names ?: "com.apple.metal.simulator") componentsSeparatedByString:@","]) {
				if (!name.length) continue;
				xpc_connection_t l = x86_xpc_connection_create_mach_service(name.UTF8String, dispatch_get_main_queue(), XPC_CONNECTION_MACH_SERVICE_LISTENER);
				if (!l) continue;
				xpc_connection_set_event_handler(l, ^(xpc_object_t peer) {
					if (xpc_get_type(peer) == XPC_TYPE_CONNECTION) {
						xpc_connection_set_event_handler(peer, ^(xpc_object_t e) { (void)e; });
						xpc_connection_resume(peer);
						xpc_connection_cancel(peer);
					}
				});
				xpc_connection_resume(l);
			}
			dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
			dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), 10 * NSEC_PER_SEC, NSEC_PER_SEC);
			dispatch_source_set_event_handler(t, ^{
				char path[256];
				const char* sp = getenv("X86CODE_MTLVK_SOCKET");
				snprintf(path, sizeof(path), "/Volumes/SystemRoot%s", sp ? sp : VKMTL_SOCKET_DEFAULT);
				int s = socket(AF_UNIX, SOCK_STREAM, 0);
				struct sockaddr_un a = { 0 };
				a.sun_family = AF_UNIX;
				strlcpy(a.sun_path, path, sizeof(a.sun_path));
				if (connect(s, (struct sockaddr*)&a, sizeof(a)) == 0) { fprintf(stderr, "metalhostd: mtlvk is back, restarting\n"); _exit(0); }
				close(s);
			});
			dispatch_resume(t);
			dispatch_main();
		}
		uint64_t registryID = dev.registryID;

		const char* names = getenv("X86CODE_METAL_NAMES");
		NSArray* list = [@(names ?: "com.apple.metal.simulator") componentsSeparatedByString:@","];
		for (NSString* name in list) {
			if (!name.length) continue;
			xpc_connection_t l = x86_xpc_connection_create_mach_service(name.UTF8String, dispatch_get_main_queue(), XPC_CONNECTION_MACH_SERVICE_LISTENER);
			if (!l) { fprintf(stderr, "metalhostd: cannot listen on %s\n", name.UTF8String); continue; }
			init_with_xpc_connection(l, registryID, 0);
			fprintf(stderr, "metalhostd: serving %s (device \"%s\", registryID %#llx)\n", name.UTF8String, dev.name.UTF8String, registryID);
		}
	}
	dispatch_main();
}
