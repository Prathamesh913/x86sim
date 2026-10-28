// x86Code: iosprobe -- test client for iosurfaced (com.apple.IOSurface.Remote), two processes.
//   iosprobe serve : process A. Creates a 64x64 BGRA surface, fills it, serves com.x86code.iosprobe:
//                    "get" -> {id, surface (IOSurfaceCreateXPCObject)}, "check" -> verifies B's writes, writes again
//   iosprobe       : process B. Looks the surface up by ID and from the XPC object, checks A's pixels, writes
//                    its own, asks A to check, re-reads A's second write; lock seeds, values, use counts.
// Prints PASS/FAIL lines and "SUMMARY n passed m failed" (B).
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <xpc/xpc.h>

extern xpc_connection_t x86_xpc_connection_create_mach_service(const char* name, dispatch_queue_t q, uint64_t flags)
	__asm__("_xpc_connection_create_mach_service");
extern xpc_object_t IOSurfaceCreateXPCObject(IOSurfaceRef);
extern IOSurfaceRef IOSurfaceLookupFromXPCObject(xpc_object_t);

static int npass, nfail;
#define CHECK(c, ...) do { if (c) { npass++; printf("PASS "); } else { nfail++; printf("FAIL "); } printf(__VA_ARGS__); printf("\n"); } while (0)
#define W 64
#define H 64

static uint32_t pat(int who, int x, int y) { return (uint32_t)(0xff000000u | (who << 16) | ((y & 0xff) << 8) | (x & 0xff)); }
static int fill(IOSurfaceRef s, int who, int y0, int y1) {
	uint32_t seed = 0;
	IOSurfaceLock(s, 0, &seed);
	uint8_t* b = IOSurfaceGetBaseAddress(s);
	size_t bpr = IOSurfaceGetBytesPerRow(s);
	for (int y = y0; y < y1; y++) for (int x = 0; x < W; x++) ((uint32_t*)(b + y * bpr))[x] = pat(who, x, y);
	IOSurfaceUnlock(s, 0, &seed);
	return (int)seed;
}
static int wrong(IOSurfaceRef s, int who, int y0, int y1, uint32_t* seedOut) {
	uint32_t seed = 0;
	IOSurfaceLock(s, kIOSurfaceLockReadOnly, &seed);
	uint8_t* b = IOSurfaceGetBaseAddress(s);
	size_t bpr = IOSurfaceGetBytesPerRow(s);
	int n = 0;
	for (int y = y0; y < y1; y++) for (int x = 0; x < W; x++) n += ((uint32_t*)(b + y * bpr))[x] != pat(who, x, y);
	IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, &seed);
	if (seedOut) *seedOut = seed;
	return n;
}

static int finish(void) { printf("SUMMARY %d passed %d failed\n", npass, nfail); return 0; }

static int serve(void) {
	NSDictionary* props = @{ (id)kIOSurfaceWidth: @W, (id)kIOSurfaceHeight: @H, (id)kIOSurfaceBytesPerElement: @4,
	                         (id)kIOSurfacePixelFormat: @((int)'BGRA') };
	IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)props);
	printf("A: IOSurfaceCreate -> %p id %u bpr %zu alloc %zu base %p\n", s, s ? IOSurfaceGetID(s) : 0,
	       s ? IOSurfaceGetBytesPerRow(s) : 0, s ? IOSurfaceGetAllocSize(s) : 0, s ? IOSurfaceGetBaseAddress(s) : NULL);
	if (!s) return 1;
	fill(s, 0xA, 0, H);
	IOSurfaceSetValue(s, CFSTR("x86code"), CFSTR("hello from A"));
	xpc_connection_t l = x86_xpc_connection_create_mach_service("com.x86code.iosprobe", dispatch_get_main_queue(), XPC_CONNECTION_MACH_SERVICE_LISTENER);
	xpc_connection_set_event_handler(l, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
		xpc_connection_set_event_handler(peer, ^(xpc_object_t m) {
			if (xpc_get_type(m) != XPC_TYPE_DICTIONARY) return;
			xpc_object_t r = xpc_dictionary_create_reply(m);
			const char* op = xpc_dictionary_get_string(m, "op");
			if (op && !strcmp(op, "get")) {
				xpc_dictionary_set_uint64(r, "id", IOSurfaceGetID(s));
				xpc_object_t x = IOSurfaceCreateXPCObject(s);
				printf("A: IOSurfaceCreateXPCObject -> %p\n", x);
				if (x) xpc_dictionary_set_value(r, "surface", x);
			} else if (op && !strcmp(op, "check")) {
				uint32_t seed = 0;
				int n = wrong(s, 0xB, H / 2, H, &seed);
				xpc_dictionary_set_int64(r, "wrong", n);
				xpc_dictionary_set_uint64(r, "seed", seed);
				xpc_dictionary_set_bool(r, "inuse", IOSurfaceIsInUse(s));
				printf("A: B's half has %d wrong texels, seed %u, in use %d\n", n, seed, IOSurfaceIsInUse(s));
				fill(s, 0xC, 0, H / 2);
			}
			xpc_connection_send_message(peer, r);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(l);
	printf("A: serving com.x86code.iosprobe\n");
	dispatch_main();
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc > 1 && !strcmp(argv[1], "serve")) return serve();
	xpc_connection_t c = x86_xpc_connection_create_mach_service("com.x86code.iosprobe", NULL, 0);
	xpc_connection_set_event_handler(c, ^(xpc_object_t e) { (void)e; });
	xpc_connection_resume(c);
	xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_string(m, "op", "get");
	xpc_object_t r = xpc_connection_send_message_with_reply_sync(c, m);
	if (xpc_get_type(r) != XPC_TYPE_DICTIONARY) { CHECK(0, "reply from process A (%s)", xpc_copy_description(r)); return finish(); }
	uint32_t sid = (uint32_t)xpc_dictionary_get_uint64(r, "id");
	xpc_object_t xs = xpc_dictionary_get_value(r, "surface");
	CHECK(sid && xs, "process A sent surface id %u and an XPC object (%s)", sid, xs ? "mach send" : "none");

	IOSurfaceRef byID = IOSurfaceLookup(sid);
	CHECK(byID && IOSurfaceGetID(byID) == sid && IOSurfaceGetWidth(byID) == W && IOSurfaceGetHeight(byID) == H &&
	      IOSurfaceGetPixelFormat(byID) == 'BGRA', "IOSurfaceLookup(%u): %p %zux%zu bpr %zu", sid, byID,
	      byID ? IOSurfaceGetWidth(byID) : 0, byID ? IOSurfaceGetHeight(byID) : 0, byID ? IOSurfaceGetBytesPerRow(byID) : 0);
	IOSurfaceRef byX = xs ? IOSurfaceLookupFromXPCObject(xs) : NULL;
	CHECK(byX && IOSurfaceGetID(byX) == sid, "IOSurfaceLookupFromXPCObject: %p id %u", byX, byX ? IOSurfaceGetID(byX) : 0);
	if (!byID) return finish();
	int n = wrong(byID, 0xA, 0, H, NULL);
	CHECK(n == 0, "B reads A's pixels via lookup by ID: %d of %d wrong", n, W * H);
	if (byX) {
		n = wrong(byX, 0xA, 0, H, NULL);
		CHECK(n == 0, "B reads A's pixels via the XPC object: %d wrong", n);
	}
	CFTypeRef v = IOSurfaceCopyValue(byID, CFSTR("x86code"));
	CHECK(v && CFEqual(v, CFSTR("hello from A")), "value set by A: %s", v ? [[(__bridge id)v description] UTF8String] : "(none)");

	uint32_t s0 = 0;
	IOSurfaceLock(byID, kIOSurfaceLockReadOnly, &s0);
	IOSurfaceUnlock(byID, kIOSurfaceLockReadOnly, NULL);
	int s1 = fill(byID, 0xB, H / 2, H);
	uint32_t s2 = 0;
	IOSurfaceLock(byID, kIOSurfaceLockReadOnly, &s2);
	IOSurfaceUnlock(byID, kIOSurfaceLockReadOnly, NULL);
	CHECK(s2 > s0 && (uint32_t)s1 == s2, "seed: %u before write, %d after write unlock, %u after read lock", s0, s1, s2);

	IOSurfaceIncrementUseCount(byID);
	xpc_dictionary_set_string(m, "op", "check");
	r = xpc_connection_send_message_with_reply_sync(c, m);
	int64_t aw = xpc_dictionary_get_int64(r, "wrong");
	CHECK(xpc_get_type(r) == XPC_TYPE_DICTIONARY && aw == 0, "A reads B's pixels: %lld wrong", aw);
	CHECK(xpc_dictionary_get_uint64(r, "seed") >= s2, "A sees seed %llu (B's write: %u)", xpc_dictionary_get_uint64(r, "seed"), s2);
	CHECK(xpc_dictionary_get_bool(r, "inuse"), "A sees B's use count (IsInUse)");
	IOSurfaceDecrementUseCount(byID);
	n = wrong(byID, 0xC, 0, H / 2, NULL);
	CHECK(n == 0, "B reads A's second write: %d wrong", n);
	CHECK(byX == NULL || IOSurfaceGetBaseAddress(byX) != NULL, "XPC-object mapping base %p, ID mapping base %p",
	      byX ? IOSurfaceGetBaseAddress(byX) : NULL, IOSurfaceGetBaseAddress(byID));

	// a planar surface of B's own (layout from the server)
	NSDictionary* p2 = @{ (id)kIOSurfaceWidth: @16, (id)kIOSurfaceHeight: @8, (id)kIOSurfacePixelFormat: @((int)'420v'),
		(id)kIOSurfacePlaneInfo: @[ @{ (id)kIOSurfacePlaneWidth: @16, (id)kIOSurfacePlaneHeight: @8, (id)kIOSurfacePlaneBytesPerElement: @1 },
		                            @{ (id)kIOSurfacePlaneWidth: @8, (id)kIOSurfacePlaneHeight: @4, (id)kIOSurfacePlaneBytesPerElement: @2 } ] };
	IOSurfaceRef pl = IOSurfaceCreate((__bridge CFDictionaryRef)p2);
	CHECK(pl && IOSurfaceGetPlaneCount(pl) == 2 && IOSurfaceGetWidthOfPlane(pl, 1) == 8 &&
	      IOSurfaceGetBytesPerElementOfPlane(pl, 1) == 2 && IOSurfaceGetBaseAddressOfPlane(pl, 1) > IOSurfaceGetBaseAddressOfPlane(pl, 0),
	      "planar surface: %zu planes, plane 1 %zux%zu bpe %zu offset %ld", pl ? IOSurfaceGetPlaneCount(pl) : 0,
	      pl ? IOSurfaceGetWidthOfPlane(pl, 1) : 0, pl ? IOSurfaceGetHeightOfPlane(pl, 1) : 0, pl ? IOSurfaceGetBytesPerElementOfPlane(pl, 1) : 0,
	      pl ? (long)((char*)IOSurfaceGetBaseAddressOfPlane(pl, 1) - (char*)IOSurfaceGetBaseAddressOfPlane(pl, 0)) : 0);
	if (pl) CFRelease(pl);
	CHECK(IOSurfaceLookup(0x7ffffff0) == NULL, "lookup of an unknown ID fails");
	return finish();
}
