// x86Code: sync XPC to a missing Mach service after earlier traffic on the same connection,
// and via NSXPCConnection synchronousRemoteObjectProxy (SpringBoard -> systemstatus.publisher)
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <xpc/xpc.h>
#include <pthread.h>
#include <time.h>
extern xpc_connection_t xpc_connection_create_mach_service_x(const char*, dispatch_queue_t, uint64_t) __asm__("_xpc_connection_create_mach_service");
@protocol XProbe - (void)ping:(NSString*)s reply:(void (^)(void))r; @end
static void* watchdog(void* k) { sleep(5); printf("STILL BLOCKED after 5s: case %ld\n", (long)k); return NULL; }
static double now(void) { struct timespec a; clock_gettime(CLOCK_MONOTONIC, &a); return a.tv_sec + a.tv_nsec / 1e9; }
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	pthread_t t;
	for (long k = 0; k < 3; k++) {
		pthread_create(&t, NULL, watchdog, (void*)k);
		xpc_connection_t c = xpc_connection_create_mach_service_x("com.x86code.missing", dispatch_get_global_queue(0, 0), 0);
		xpc_connection_set_event_handler(c, ^(xpc_object_t e) { printf("  event %s\n", e == XPC_ERROR_CONNECTION_INVALID ? "INVALID" : "other"); });
		xpc_connection_resume(c);
		xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
		if (k == 0) xpc_connection_send_message(c, m);
		if (k == 1) xpc_connection_send_message_with_reply(c, m, NULL, ^(xpc_object_t r) { printf("  async reply\n"); });
		if (k == 2) { (void)xpc_connection_send_message_with_reply_sync(c, m); }
		usleep(200000);
		double a = now();
		xpc_object_t r = xpc_connection_send_message_with_reply_sync(c, m);
		pthread_cancel(t);
		printf("case %ld: %.3fs %s\n", k, now() - a, r == XPC_ERROR_CONNECTION_INVALID ? "INVALID" : "other");
	}
	{
		// NSXPC: connection queue set to a private serial queue, sync proxy call from that queue
		// (SystemStatus publisher: _setQueue: internalqueue, sync call on internalqueue)
		long k = 8;
		dispatch_queue_t q = dispatch_queue_create("x86code.internalqueue", NULL);
		pthread_create(&t, NULL, watchdog, (void*)k);
		NSXPCConnection* c = ((NSXPCConnection*(*)(id, SEL, NSString*, NSUInteger))objc_msgSend)([NSXPCConnection alloc], sel_registerName("initWithMachServiceName:options:"), @"com.x86code.missing", 0);
		c.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(XProbe)];
		((void(*)(id, SEL, dispatch_queue_t))objc_msgSend)(c, sel_registerName("_setQueue:"), q);
		[c resume];
		dispatch_sync(q, ^{ double a = now(); __block int err = 0;
			[[c synchronousRemoteObjectProxyWithErrorHandler:^(NSError* e) { err = (int)e.code; }] ping:@"x" reply:^{}];
			pthread_cancel(t); printf("case 8 (NSXPC own queue): %.3fs err %d\n", now() - a, err); });
	}
	for (long k = 7; k >= 6; k--) {
		// sync send from a block running on the connection's own target queue (main / serial)
		dispatch_queue_t q = k == 6 ? dispatch_get_main_queue() : dispatch_queue_create("x86code.serial", NULL);
		pthread_create(&t, NULL, watchdog, (void*)k);
		xpc_connection_t c = xpc_connection_create_mach_service_x("com.x86code.missing", q, 0);
		xpc_connection_set_event_handler(c, ^(xpc_object_t e) {});
		xpc_connection_resume(c);
		dispatch_semaphore_t s = dispatch_semaphore_create(0);
		dispatch_async(q, ^{ double a = now(); xpc_object_t r = xpc_connection_send_message_with_reply_sync(c, xpc_dictionary_create(NULL, NULL, 0));
			pthread_cancel(t); printf("case %ld: %.3fs %s\n", k, now() - a, r == XPC_ERROR_CONNECTION_INVALID ? "INVALID" : "other"); dispatch_semaphore_signal(s); });
		if (k == 6) { while (dispatch_semaphore_wait(s, DISPATCH_TIME_NOW)) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true); }
		else dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER);
	}
	for (long k = 3; k < 6; k++) {
		pthread_create(&t, NULL, watchdog, (void*)k);
		NSXPCConnection* c = ((NSXPCConnection*(*)(id, SEL, NSString*, NSUInteger))objc_msgSend)([NSXPCConnection alloc], sel_registerName("initWithMachServiceName:options:"), @"com.x86code.missing", 0);
		c.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(XProbe)];
		if (k == 5) c.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(XProbe)];
		[c resume];
		if (k == 4) [[c remoteObjectProxy] ping:@"a" reply:^{}];
		usleep(200000);
		double a = now();
		__block int err = 0;
		[[c synchronousRemoteObjectProxyWithErrorHandler:^(NSError* e) { err = (int)e.code; }] ping:@"x" reply:^{ printf("  replied\n"); }];
		pthread_cancel(t);
		printf("case %ld (NSXPC): %.3fs err %d\n", k, now() - a, err);
	}
	return 0;
}
