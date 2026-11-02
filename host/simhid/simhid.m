// x86Code: stand-in for CoreSimulator's SimulatorHID.framework (x86_64, iOS simulator), loaded by backboardd.
//
// How the runtime uses it (SimulatorClient.framework, IndigoHIDSystemSpawnLoopback(IOHIDEventSystemRef)):
//   path = getenv("SIMULATOR_HID_SYSTEM_MANAGER"); bundle = [NSBundle bundleWithPath:path];
//   cls = [bundle principalClass]  (Info.plist NSPrincipalClass "SimHIDSystem"), must conform to the protocol
//   SimulatorClientHIDSystemManager; manager = [[cls alloc] initUsingHIDService:<backboardd's IOHIDEventSystemRef>
//   error:&err] and is kept forever.
// Apple's SimHIDSystem receives "Indigo" HID messages from Simulator.app over a mach port and dispatches them
// through virtual HID services it adds to that event system (_IOHIDServiceCreateVirtual + _IOHIDEventSystemAddService
// + _IOHIDServiceDispatchEvent). We do the same with a built-in touchscreen, the main-screen buttons and a keyboard,
// fed by a Unix socket (simview / cli/libexec/simtouch.py on Linux) instead of Indigo messages.
//
// Socket: $SIMHID_SOCKET (default <SIMULATOR_SHARED_RESOURCES_DIRECTORY>/simhid.sock), stream, any number of
// clients, one text command per line:
//   touch down|move|up <x> <y> [edge]   x, y normalized to the display (0..1, origin top left); edge (on down, kept
//                                  for the whole touch): none|top|left|bottom|right, a touch that begins at that screen
//                                  edge (system gestures: home indicator, Control/Notification Center)
//   touch2 down|move|up <x1> <y1> <x2> <y2>   two fingers (pinch, two-finger pan): one hand event with two finger
//                                  children, finger indexes 1 and 2 with their own identities
//   key down|up <usage>            keyboard usage (page 7), e.g. 4 = 'a', 0x28 = return
//   button home|lock|volup|voldown down|up
#import <Foundation/Foundation.h>
typedef int IOReturn;
typedef uint32_t IOOptionBits;
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>

typedef struct __IOHIDEvent* IOHIDEventRef;
typedef struct __IOHIDService* IOHIDServiceRef;
typedef struct __IOHIDEventSystem* IOHIDEventSystemRef_;
typedef double IOHIDFloat;

// IOKit private (IOHIDFamily: IOHIDServicePrivate.h / IOHIDEventSystemPrivate.h). The callback table layout was read
// from the runtime's IOKit (__IOHIDServiceCreateVirtualNoInit copies 0x58 bytes when version == 2; the service calls
// +0x8 open, +0x10 close, +0x18 copyProperty, +0x20 setProperty, +0x28 setEventCallback, +0x30/+0x38 schedule/
// unschedule, +0x40 copyEvent, +0x48 setOutputEvent, +0x50 copyMatchingEvent).
typedef struct {
	uint64_t version;
	Boolean (*open)(void* target, void* refcon, IOOptionBits options);
	void (*close)(void* target, void* refcon, IOOptionBits options);
	CFTypeRef (*copyProperty)(void* target, void* refcon, CFStringRef key);
	Boolean (*setProperty)(void* target, void* refcon, CFStringRef key, CFTypeRef value);
	void (*setEventCallback)(void* target, void* refcon, void* callback, void* cbTarget, void* cbRefcon);
	void (*schedule)(void* target, void* refcon, dispatch_queue_t queue);
	void (*unschedule)(void* target, void* refcon, dispatch_queue_t queue);
	IOHIDEventRef (*copyEvent)(void* target, void* refcon, uint32_t type, IOHIDEventRef matching, IOOptionBits options);
	IOReturn (*setOutputEvent)(void* target, void* refcon, IOHIDEventRef event);
	IOHIDEventRef (*copyMatchingEvent)(void* target, void* refcon, CFDictionaryRef matching);
} VirtualCallbacks;
extern IOHIDServiceRef _IOHIDServiceCreateVirtual(CFAllocatorRef, uint64_t serviceID, const VirtualCallbacks*, void* target, void* refcon);
extern void _IOHIDEventSystemAddService(IOHIDEventSystemRef_, IOHIDServiceRef);
extern void _IOHIDServiceDispatchEvent(IOHIDServiceRef, IOHIDEventRef);
extern IOHIDEventRef IOHIDEventCreateDigitizerEvent(CFAllocatorRef, uint64_t ts, uint32_t transducerType, uint32_t index,
	uint32_t identity, uint32_t eventMask, uint32_t buttonMask, IOHIDFloat x, IOHIDFloat y, IOHIDFloat z,
	IOHIDFloat tipPressure, IOHIDFloat barrelPressure, Boolean range, Boolean touch, IOOptionBits options);
extern IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(CFAllocatorRef, uint64_t ts, uint32_t index, uint32_t identity,
	uint32_t eventMask, IOHIDFloat x, IOHIDFloat y, IOHIDFloat z, IOHIDFloat tipPressure, IOHIDFloat twist,
	Boolean range, Boolean touch, IOOptionBits options);
extern IOHIDEventRef IOHIDEventCreateKeyboardEvent(CFAllocatorRef, uint64_t ts, uint32_t usagePage, uint32_t usage,
	Boolean down, IOOptionBits flags);
extern void IOHIDEventAppendEvent(IOHIDEventRef, IOHIDEventRef, IOOptionBits);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef, uint32_t field, CFIndex value);

#define kDigitizerTransducerHand 3
#define kDigitizerRange 0x1
#define kDigitizerTouch 0x2
#define kDigitizerPosition 0x4
#define kDigitizerIdentity 0x20
#define kFieldDigitizerIsDisplayIntegrated ((11 << 16) | 25)
// x86Code: how Apple's Simulator marks edge touches (SimulatorKit IndigoHIDMessageForMouseNSEvent, Xcode 27.1): the
// IndigoHIDEdge (0 none, 1 top, 2 left, 3 bottom, 4 right) picks the digitizer event mask from a table, 0x40000 |
// {0, 0x02000000, 0x08000000, 0x01000000, 0x04000000} (the swipe direction away from that edge: top = swipe down,
// bottom = swipe up ...), and SimulatorHID passes that mask to IOHIDEventCreateDigitizerFingerEvent on every phase.
// SpringBoard's system gestures (home indicator, Control/Notification Center) only claim touches carrying these bits.
static const uint32_t edge_mask[5] = { 0x40000, 0x02040000, 0x08040000, 0x01040000, 0x04040000 };
static int touch_edge;

// x86Code arm64: also mirror to a host-visible log file. stderr under the JIT can be late or
// interleaved with backboardd's own logging; a dedicated file lets us see whether the socket
// thread actually runs handle_line / dispatch.
static FILE* g_logf;
static void slog(const char* fmt, ...) {
	va_list ap; va_start(ap, fmt);
	fprintf(stderr, "simhid: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
	if (!g_logf) {
		const char* p = getenv("SIMHID_LOG");
		if (!p || !*p) p = "/Volumes/SystemRoot/home/bobby/scratch/x86code/simhid.log";
		g_logf = fopen(p, "a");
		if (g_logf) setvbuf(g_logf, NULL, _IOLBF, 0);
	}
	if (g_logf) {
#if __arm64__
		// wall-clock ns (same clock as the frame ring's slot t_ns) so touches line up with frames (simrecord.py)
		{ struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); fprintf(g_logf, "%llu ", (unsigned long long)ts.tv_sec * 1000000000ull + ts.tv_nsec); }
#endif
		fprintf(g_logf, "simhid: ");
		va_list ap2; va_start(ap2, fmt);
		vfprintf(g_logf, fmt, ap2);
		va_end(ap2);
		fprintf(g_logf, "\n");
		fflush(g_logf);
	}
	va_end(ap);
}

typedef struct {
	const char* name;
	uint64_t id;
	CFDictionaryRef props;
	IOHIDServiceRef service;
	dispatch_queue_t queue;
} VService;

static VService touch_svc, buttons_svc, keyboard_svc;
static IOHIDEventSystemRef_ hid_system;

static Boolean v_open(void* t, void* r, IOOptionBits o) {
	// Speed 34: NULL-safe: after the idle detach (_IOHIDServiceTerminate) IOKit calls close / unschedule with a NULL
	// target (backboardd crashed in v_close on the first gate)
	slog("v_open %s", t ? ((VService*)t)->name : "(null)");
	return true;
}
static void v_close(void* t, void* r, IOOptionBits o) { slog("v_close %s", t ? ((VService*)t)->name : "(null)"); }
static CFTypeRef v_copyProperty(void* t, void* r, CFStringRef key) {
	VService* s = t;
	if (!s || !s->props) return NULL;
	CFTypeRef v = CFDictionaryGetValue(s->props, key);
	return v ? CFRetain(v) : NULL;
}
static Boolean v_setProperty(void* t, void* r, CFStringRef key, CFTypeRef value) { return true; }
static void v_setEventCallback(void* t, void* r, void* cb, void* a, void* b) {
	slog("v_setEventCallback %s cb=%p", t ? ((VService*)t)->name : "(null)", cb);
}
static void v_schedule(void* t, void* r, dispatch_queue_t q) {
	VService* s = t;
	if (!s) return;
	if (q) dispatch_retain(q);
	s->queue = q;
	slog("v_schedule %s queue=%p", s->name, q);
}
static void v_unschedule(void* t, void* r, dispatch_queue_t q) {
	if (!t) { slog("v_unschedule (null)"); return; }
	slog("v_unschedule %s", ((VService*)t)->name);
	((VService*)t)->queue = NULL;
}
static IOHIDEventRef v_copyEvent(void* t, void* r, uint32_t type, IOHIDEventRef m, IOOptionBits o) { return NULL; }
static IOReturn v_setOutputEvent(void* t, void* r, IOHIDEventRef e) { return 0; }
static IOHIDEventRef v_copyMatchingEvent(void* t, void* r, CFDictionaryRef m) { return NULL; }

static const VirtualCallbacks callbacks = { 2, v_open, v_close, v_copyProperty, v_setProperty, v_setEventCallback,
	v_schedule, v_unschedule, v_copyEvent, v_setOutputEvent, v_copyMatchingEvent };

static CFDictionaryRef make_props(NSString* product, int page, int usage, NSDictionary* extra) {
	NSMutableDictionary* d = [NSMutableDictionary dictionaryWithDictionary:@{
		@"PrimaryUsagePage": @(page), @"PrimaryUsage": @(usage),
		@"DeviceUsagePairs": @[ @{ @"DeviceUsagePage": @(page), @"DeviceUsage": @(usage) } ],
		@"Product": product, @"Transport": @"Virtual", @"Built-In": @YES, @"Authenticated": @YES,
		@"VendorID": @0x5ac, @"ProductID": @0, @"LocationID": @0 }];
	if (extra) [d addEntriesFromDictionary:extra];
	return (CFDictionaryRef)[d retain];
}

static void add_service(IOHIDEventSystemRef_ sys, VService* s) {
	s->service = _IOHIDServiceCreateVirtual(kCFAllocatorDefault, s->id, &callbacks, s, NULL);
	if (!s->service) { slog("could not create %s", s->name); return; }
	_IOHIDEventSystemAddService(sys, s->service);
	slog("added %s service (id 0x%llx)", s->name, (unsigned long long)s->id);
}

#if __arm64__
// Speed 34: detach the keyboard service again after SIMHID_KB_IDLE_S (default 30; 0 = never) without a key, as a
// hardware keyboard that goes away. While one is attached UIKit runs in hardware-keyboard mode, and SpringBoard's
// focus system then walks the focus environment on every app transition: +80-100 ms of SpringBoard CPU per warm
// Settings tap (630-720 ms vs 530-550 fresh), which is what "the aged system" was (the gate types into Spotlight /
// Settings search, and the attach stayed for the rest of the boot). The next key attaches it again (with the settle).
extern void _IOHIDEventSystemRemoveService(IOHIDEventSystemRef_, IOHIDServiceRef);
extern void _IOHIDServiceTerminate(IOHIDServiceRef);
static uint64_t kb_last_ns;
static int kb_down;  // keys held (no detach while one is down)
static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ull + ts.tv_nsec; }
static pthread_mutex_t cmd_mu;
static void* kb_idle_thread(void* arg) {
	const uint64_t idle = (uint64_t)(intptr_t)arg * 1000000000ull;
	for (;;) {
		sleep(2);
		pthread_mutex_lock(&cmd_mu);
		if (keyboard_svc.service && hid_system && kb_down <= 0 && now_ns() - kb_last_ns > idle) {
			IOHIDServiceRef sv = keyboard_svc.service;
			keyboard_svc.service = NULL;
			_IOHIDEventSystemRemoveService(hid_system, sv);
			_IOHIDServiceTerminate(sv);
			CFRelease(sv);
			kb_down = 0;
			slog("keyboard detached (idle)");
		}
		pthread_mutex_unlock(&cmd_mu);
	}
	return NULL;
}
#endif

static void dispatch_event(VService* s, IOHIDEventRef e) {
	if (!s->service || !e) {
		slog("dispatch_event drop service=%p e=%p", s ? s->service : NULL, e);
		if (e) CFRelease(e);
		return;
	}
	// x86Code arm64: call _IOHIDServiceDispatchEvent on this thread. dispatch_async onto the
	// service queue / main queue was a silent no-op under the JIT for socket-thread producers
	// (reads happened, UI never saw touches). Direct dispatch matches Indigo's in-thread path.
	_IOHIDServiceDispatchEvent(s->service, e);
	CFRelease(e);
}

// --- events

static bool touching;
static uint32_t touch_identity = 2;

static void send_touch(const char* phase, double x, double y, int edge) {
	bool down = !strcmp(phase, "down"), up = !strcmp(phase, "up"), move = !strcmp(phase, "move");
	if (!down && !up && !move) return;
	if (move && !touching) return;
	if (down) { if (touching) send_touch("up", x, y, 0); touching = true; touch_identity++; touch_edge = edge; }
	// (a single-finger down while two fingers are down: the two-finger contact is lifted first, see send_touch2)
	if (up) { if (!touching) return; touching = false; }
	uint64_t ts = mach_absolute_time();
	uint32_t mask = kDigitizerPosition | (down || up ? kDigitizerTouch | kDigitizerRange | kDigitizerIdentity : 0);
	mask |= edge_mask[touch_edge];
	bool in = !up;
	IOHIDEventRef hand = IOHIDEventCreateDigitizerEvent(kCFAllocatorDefault, ts, kDigitizerTransducerHand, 0, 0, mask, 0,
		x, y, 0, in ? 1 : 0, 0, in, in, 0);
	IOHIDEventSetIntegerValue(hand, kFieldDigitizerIsDisplayIntegrated, 1);
	IOHIDEventRef finger = IOHIDEventCreateDigitizerFingerEvent(kCFAllocatorDefault, ts, 1, touch_identity, mask,
		x, y, 0, in ? 1 : 0, 0, in, in, 0);
	IOHIDEventSetIntegerValue(finger, kFieldDigitizerIsDisplayIntegrated, 1);
	IOHIDEventAppendEvent(hand, finger, 0);
	CFRelease(finger);
	slog("touch %s (%.4f,%.4f) edge=%d svc=%p queue=%p", phase, x, y, touch_edge, touch_svc.service, touch_svc.queue);
	dispatch_event(&touch_svc, hand);
}

// x86Code: two fingers in one hand event, as a multi-touch digitizer reports them (the hand carries the centroid; a
// finger child per contact with its own index and identity; range/touch/identity bits on the down and up events).
static bool touching2;
static uint32_t touch2_identity = 1000;
static void send_touch2(const char* phase, double x1, double y1, double x2, double y2) {
	bool down = !strcmp(phase, "down"), up = !strcmp(phase, "up"), move = !strcmp(phase, "move");
	if (!down && !up && !move) return;
	if (move && !touching2) return;
	if (down) {
		if (touching) send_touch("up", x1, y1, 0);
		if (touching2) send_touch2("up", x1, y1, x2, y2);
		touching2 = true; touch2_identity += 2;
	}
	if (up) { if (!touching2) return; touching2 = false; }
	uint64_t ts = mach_absolute_time();
	uint32_t mask = kDigitizerPosition | (down || up ? kDigitizerTouch | kDigitizerRange | kDigitizerIdentity : 0);
	mask |= edge_mask[0];
	bool in = !up;
	IOHIDEventRef hand = IOHIDEventCreateDigitizerEvent(kCFAllocatorDefault, ts, kDigitizerTransducerHand, 0, 0, mask, 0,
		(x1 + x2) / 2, (y1 + y2) / 2, 0, in ? 1 : 0, 0, in, in, 0);
	IOHIDEventSetIntegerValue(hand, kFieldDigitizerIsDisplayIntegrated, 1);
	double xs[2] = { x1, x2 }, ys[2] = { y1, y2 };
	for (int i = 0; i < 2; i++) {
		IOHIDEventRef finger = IOHIDEventCreateDigitizerFingerEvent(kCFAllocatorDefault, ts, i + 1, touch2_identity + i, mask,
			xs[i], ys[i], 0, in ? 1 : 0, 0, in, in, 0);
		IOHIDEventSetIntegerValue(finger, kFieldDigitizerIsDisplayIntegrated, 1);
		IOHIDEventAppendEvent(hand, finger, 0);
		CFRelease(finger);
	}
	dispatch_event(&touch_svc, hand);
}

static void send_key(VService* s, uint32_t page, uint32_t usage, bool down) {
	dispatch_event(s, IOHIDEventCreateKeyboardEvent(kCFAllocatorDefault, mach_absolute_time(), page, usage, down, 0));
}

static void handle_line(char* line) {
	char a[16] = "", b[16] = "";
	double x = 0, y = 0;
	double x2 = 0, y2 = 0;
	if (sscanf(line, "touch2 %15s %lf %lf %lf %lf", a, &x, &y, &x2, &y2) == 5) { send_touch2(a, x, y, x2, y2); return; }
	if (sscanf(line, "touch %15s %lf %lf %15s", a, &x, &y, b) >= 3) {
		static const char* edges[5] = { "none", "top", "left", "bottom", "right" };
		int edge = 0;
		for (int i = 1; i < 5; i++) if (!strcmp(b, edges[i])) edge = i;
		send_touch(a, x, y, edge);
		return;
	}
	unsigned usage;
	if (sscanf(line, "key %15s %i", a, &usage) == 2) {
#if __arm64__
		// x86Code arm64: the keyboard service is attached at the first key command (see initUsingHIDService)
		// Speed 31: the first key used to be followed by the next ones 300 ms later, and those were lost: the new
		// keyboard puts UIKit into hardware-keyboard mode and the focused field's keyboard session restarts (the
		// software keyboard goes away) before it takes keys. The keys are not dropped here: this client's later lines
		// wait (they queue in the socket, in order) until the session had time to come back. SIMHID_KB_SETTLE_MS
		// (default 1500) after the first attach; the first key itself waits too.
		if (!keyboard_svc.service && hid_system) {
			add_service(hid_system, &keyboard_svc);
			const char* ms = getenv("SIMHID_KB_SETTLE_MS");
			usleep((ms && *ms ? atoi(ms) : 1500) * 1000);
			slog("keyboard settled");
		}
		kb_last_ns = now_ns();
		kb_down += !strcmp(a, "down") ? 1 : -1;
		if (kb_down < 0) kb_down = 0;
#endif
		send_key(&keyboard_svc, 7, usage, !strcmp(a, "down"));
		return;
	}
	if (sscanf(line, "button %15s %15s", a, b) == 2) {
		bool down = !strcmp(b, "down");
		// consumer page: 0x40 menu (home), 0x30 power (lock), 0xe9/0xea volume
		uint32_t u = !strcmp(a, "home") ? 0x40 : !strcmp(a, "lock") ? 0x30 : !strcmp(a, "volup") ? 0xe9 : !strcmp(a, "voldown") ? 0xea : 0;
		if (u) send_key(&buttons_svc, 0x0c, u, down);
		return;
	}
	if (*line) slog("unknown command: %s", line);
}

// x86Code arm64: serialize with a mutex instead of dispatch_sync. dispatch_sync from the
// socket pthread onto cmd_q was observed to consume socket bytes without ever running the
// block under the A64 JIT (no touch logs, no UI). Mutex + direct call stays on this GuestThread.
static pthread_mutex_t cmd_mu = PTHREAD_MUTEX_INITIALIZER;
static void* client_thread(void* arg) {
	int fd = (int)(intptr_t)arg;
	slog("client_thread fd=%d", fd);
	char buf[4096];
	size_t n = 0;
	for (;;) {
		ssize_t r = read(fd, buf + n, sizeof(buf) - 1 - n);
		if (r <= 0) break;
		n += r;
		buf[n] = 0;
		char* start = buf;
		char* nl;
		while ((nl = strchr(start, '\n'))) {
			*nl = 0;
			if (nl > start && nl[-1] == '\r') nl[-1] = 0;
			pthread_mutex_lock(&cmd_mu);
			handle_line(start);
			pthread_mutex_unlock(&cmd_mu);
			start = nl + 1;
		}
		n = strlen(start);
		memmove(buf, start, n);
		if (n >= sizeof(buf) - 1) n = 0;
	}
	slog("client_thread fd=%d done", fd);
	close(fd);
	return NULL;
}

static void* listen_thread(void* arg) {
	char path[256];
	const char* env = getenv("SIMHID_SOCKET");
	const char* dir = getenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY") ?: getenv("HOME") ?: "/tmp";
	if (env) snprintf(path, sizeof(path), "%s", env); else snprintf(path, sizeof(path), "%s/simhid.sock", dir);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { 0 };
	a.sun_family = AF_UNIX;
	strlcpy(a.sun_path, path, sizeof(a.sun_path));
	unlink(path);
	if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(s, 8) != 0) {
		slog("cannot listen on %s: errno %d", path, errno);
		return NULL;
	}
	slog("listening on %s", path);
	for (;;) {
		// x86Code: Darling's accept() crashes with a NULL address (it converts the sockaddr back unconditionally)
		struct sockaddr_un peer; socklen_t plen = sizeof(peer);
		int c = accept(s, (struct sockaddr*)&peer, &plen);
		if (c < 0) { if (errno == EINTR) continue; slog("accept: errno %d", errno); usleep(100000); continue; }
		pthread_t t;
		pthread_create(&t, NULL, client_thread, (void*)(intptr_t)c);
		pthread_detach(t);
	}
	return NULL;
}

@protocol SimulatorClientHIDSystemManager <NSObject>
- (instancetype)initUsingHIDService:(IOHIDEventSystemRef_)system error:(NSError**)error;
@end

@interface SimHIDSystem : NSObject <SimulatorClientHIDSystemManager>
@end

@implementation SimHIDSystem
- (instancetype)initUsingHIDService:(IOHIDEventSystemRef_)system error:(NSError**)error {
	if (!(self = [super init])) return nil;
	if (!system) { if (error) *error = [NSError errorWithDomain:@"SimHID" code:1 userInfo:nil]; [self release]; return nil; }
	// displayUUID must match SFBDisplayGetDisplayUID ("x86Code-main-display"): BKIOHIDService
	// _setUpInitialProperties copies the HID property "displayUUID" into its ivar; without it,
	// BKDisplayConvertCAScreenLocationToContext gets a null UUID and drops every touch.
	touch_svc = (VService){ "touchscreen", 0x7863000000000001ull,
		make_props(@"com.apple.SimulatorHID.ScreenTouchService.x86Code-main-display", 0x0d, 0x04, @{
			@"DisplayIntegrated": @YES,
			@"SupportsInk": @NO,
			@"displayUUID": @"x86Code-main-display",
		}) };
	buttons_svc = (VService){ "buttons", 0x7863000000000002ull, make_props(@"x86Code buttons", 0x0c, 0x01, nil) };
	keyboard_svc = (VService){ "keyboard", 0x7863000000000003ull, make_props(@"x86Code keyboard", 0x01, 0x06, nil) };
	add_service(system, &touch_svc);
	add_service(system, &buttons_svc);
#if __arm64__
	// x86Code arm64: an attached keyboard service puts UIKit in hardware-keyboard mode, so a text field never shows the
	// software keyboard. Attach it only when the host first types (like Xcode's simulator does); SIMHID_KEYBOARD=1: at start.
	hid_system = system;
	const char* kb = getenv("SIMHID_KEYBOARD");
	if (kb && *kb == '1')
		add_service(system, &keyboard_svc);
	else {  // Speed 34: idle detach of an on-demand keyboard (SIMHID_KEYBOARD=1 keeps it attached)
		const char* is = getenv("SIMHID_KB_IDLE_S");
		const int idle = is && *is ? atoi(is) : 30;
		if (idle > 0) {
			pthread_t kt;
			pthread_create(&kt, NULL, kb_idle_thread, (void*)(intptr_t)idle);
			pthread_detach(kt);
		}
	}
#else
	add_service(system, &keyboard_svc);
#endif
	pthread_t t;
	pthread_create(&t, NULL, listen_thread, NULL);
	pthread_detach(t);
	slog("SimHIDSystem ready");
	return self;
}
@end
