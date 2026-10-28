// x86Code: NSXPC (anonymous listener, same process) sending NSData of various sizes, sequential and
// concurrent; counts replies where the server got nil or a wrong length (SpringBoard got a nil
// CHSWidgetExtensionsBox "data" although containsValueForKey: said it was there).
#import <Foundation/Foundation.h>
@protocol P
- (void)take:(NSData*)d reply:(void (^)(NSUInteger))r;
@end
@interface S : NSObject <P, NSXPCListenerDelegate>
@end
@implementation S
- (void)take:(NSData*)d reply:(void (^)(NSUInteger))r { r(d ? d.length * 256 + (d.length ? ((const unsigned char*) d.bytes)[d.length - 1] : 0) : (NSUInteger) -1); }
- (BOOL)listener:(NSXPCListener*)l shouldAcceptNewConnection:(NSXPCConnection*)c
{
	c.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(P)];
	c.exportedObject = self;
	[c resume];
	return YES;
}
@end
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	S* s = [S new];
	NSXPCListener* l = [NSXPCListener anonymousListener];
	l.delegate = s;
	[l resume];
	NSXPCConnection* c = [[NSXPCConnection alloc] initWithListenerEndpoint:l.endpoint];
	c.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(P)];
	[c resume];
	id<P> px = [c synchronousRemoteObjectProxyWithErrorHandler:^(NSError* e) { printf("error %s\n", e.description.UTF8String); }];
	static size_t sizes[] = { 100, 4000, 16383, 16384, 20000, 35274, 65536, 200000, 1000000 };
	for (int k = 0; k < sizeof(sizes) / sizeof(*sizes); k++) {
		NSMutableData* d = [NSMutableData dataWithLength:sizes[k]];
		memset(d.mutableBytes, 'a' + k, sizes[k]);
		__block int bad = 0, n = 200;
		for (int i = 0; i < n; i++)
			[px take:d reply:^(NSUInteger len) { if (len != sizes[k] * 256 + 'a' + k) { if (bad++ < 3) printf("  size %zu got %ld last byte %c\n", sizes[k], (long) len / 256, (int) (len % 256)); } }];
		printf("sequential size %zu: %d/%d bad\n", sizes[k], bad, n);
		__block int cbad = 0;
		dispatch_apply(200, dispatch_get_global_queue(0, 0), ^(size_t i) {
			[px take:d reply:^(NSUInteger len) { if (len != sizes[k] * 256 + 'a' + k) __sync_fetch_and_add(&cbad, 1); }];
		});
		printf("concurrent size %zu: %d/200 bad\n", sizes[k], cbad);
	}
	printf("done\n");
	return 0;
}
