// x86Code: stand-in for CoreSimulator's SimFramebuffer.framework (x86_64, iOS simulator).
// There is no host: one built-in display; every submitted frame is copied to
// a shared-memory ring (see "frame output" below; cli/libexec/fbring.py) and, every 2 s, to $SIMFB_OUT (default
// $HOME/framebuffer.bgra) as a 16-byte header {w, h, stride, seq}
// followed by BGRA rows. API and semantics: see SimFramebuffer.h.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <Block.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

#define EXPORT __attribute__((visibility("default")))

#define WIDTH 1179
#define HEIGHT 2556

typedef struct __attribute__((packed)) {
	uint32_t mode_id, width, height, unknown_0c, unknown_10, pixel_format, colorspace, refresh_centihz;
	uint8_t is_preferred, unknown_21, is_current;
} SFBDisplayMode;

// CF runtime (private, stable)
typedef struct {
	CFIndex version;
	const char* className;
	void (*init)(CFTypeRef);
	CFTypeRef (*copy)(CFAllocatorRef, CFTypeRef);
	void (*finalize)(CFTypeRef);
	Boolean (*equal)(CFTypeRef, CFTypeRef);
	CFHashCode (*hash)(CFTypeRef);
	CFStringRef (*copyFormattingDesc)(CFTypeRef, CFDictionaryRef);
	CFStringRef (*copyDebugDesc)(CFTypeRef);
	void* reclaim;
	void* refcount;
	uintptr_t requiredAlignment;
} CFRuntimeClass;
extern CFTypeID _CFRuntimeRegisterClass(const CFRuntimeClass* cls);
extern CFTypeRef _CFRuntimeCreateInstance(CFAllocatorRef allocator, CFTypeID typeID, CFIndex extraBytes, unsigned char* category);

typedef struct { uintptr_t cfisa; uint64_t cfinfo; } CFRuntimeBase;

typedef struct __SFBConnection { CFRuntimeBase base; uint64_t id; } *SFBConnectionRef;
typedef struct __SFBDisplay { CFRuntimeBase base; } *SFBDisplayRef;
struct __SFBSwapchain;
typedef void (^SFBSwapchainCallback)(CFErrorRef, struct __SFBSwapchain*, uint64_t, uint64_t, void*);
typedef struct __SFBSwapchain {
	CFRuntimeBase base;
	uint32_t width, height, presentation_mode;
	IOSurfaceRef pending;
	void* cb_context;
	dispatch_queue_t cb_queue;
	SFBSwapchainCallback cb;
	uint64_t seq;
} *SFBSwapchainRef;

static dispatch_queue_t dump_q;
static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static CFTypeID conn_type, display_type, swapchain_type;
static SFBDisplayRef the_display;
static CFStringRef display_name, display_uid;

static void swapchain_finalize(CFTypeRef ref) {
	SFBSwapchainRef sc = (SFBSwapchainRef)ref;
	if (sc->pending) CFRelease(sc->pending);
	if (sc->cb) Block_release(sc->cb);
}

static void init_types(void) {
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		static const CFRuntimeClass c = { 0, "SFBConnection" }, d = { 0, "SFBDisplay" },
			s = { 0, "SFBSwapchain", NULL, NULL, swapchain_finalize };
		conn_type = _CFRuntimeRegisterClass(&c);
		display_type = _CFRuntimeRegisterClass(&d);
		swapchain_type = _CFRuntimeRegisterClass(&s);
		the_display = (SFBDisplayRef)_CFRuntimeCreateInstance(NULL, display_type, sizeof(*the_display) - sizeof(CFRuntimeBase), NULL);
		display_name = CFSTR("LCD"); // CA clients pick the main display by this exact name
		display_uid = CFSTR("x86Code-main-display");
	});
}

static void fill_mode(SFBDisplayMode* m) {
	memset(m, 0, sizeof(*m));
	m->width = WIDTH;
	m->height = HEIGHT;
	m->pixel_format = 'BGRA';
	m->colorspace = 1;
	m->refresh_centihz = 6000;
	m->is_preferred = 1;
	m->is_current = 1;
}

// --- connection

EXPORT const char* simFramebufferGetStructName(uint32_t id) { return "unknown"; }
EXPORT CFTypeID SFBConnectionGetTypeID(void) { init_types(); return conn_type; }
EXPORT SFBConnectionRef SFBConnectionCreate(CFAllocatorRef allocator, CFStringRef name) {
	init_types();
	SFBConnectionRef c = (SFBConnectionRef)_CFRuntimeCreateInstance(allocator, conn_type, sizeof(*c) - sizeof(CFRuntimeBase), NULL);
	c->id = 1;
	return c;
}
EXPORT bool SFBConnectionConnect(SFBConnectionRef c, CFErrorRef* error) { return true; }
EXPORT uint64_t SFBConnectionGetID(SFBConnectionRef c) { return 1; }
EXPORT CFArrayRef SFBConnectionCopyDisplays(SFBConnectionRef c, CFErrorRef* error) {
	const void* v = the_display;
	return CFArrayCreate(NULL, &v, 1, &kCFTypeArrayCallBacks);
}
EXPORT SFBDisplayRef SFBConnectionCopyDisplay(SFBConnectionRef c, uint32_t id) {
	return id == 1 ? (SFBDisplayRef)CFRetain(the_display) : NULL;
}
EXPORT SFBDisplayRef SFBConnectionCopyDisplayByUID(SFBConnectionRef c, CFStringRef uid) {
	return uid && CFEqual(uid, display_uid) ? (SFBDisplayRef)CFRetain(the_display) : NULL;
}
EXPORT void SFBConnectionSetDisplayConnectedHandler(SFBConnectionRef c, dispatch_queue_t q, void* handler) {}
EXPORT void SFBConnectionSetDisplayDisconnectedHandler(SFBConnectionRef c, dispatch_queue_t q, void* handler) {}
EXPORT void SFBConnectionSetDisplayUpdatedHandler(SFBConnectionRef c, dispatch_queue_t q, void* handler) {}
EXPORT SFBDisplayRef SFBConnectionCreateDisplay(SFBConnectionRef c, void* a, void* b, void* d, void* e) { return NULL; }
EXPORT bool SFBConnectionUpdateDisplay(SFBConnectionRef c, SFBDisplayRef d, void* a, void* b) { return false; }
EXPORT bool SFBConnectionRemoveDisplay(SFBConnectionRef c, SFBDisplayRef d, void* a) { return false; }

// --- display

EXPORT CFTypeID SFBDisplayGetTypeID(void) { init_types(); return display_type; }
EXPORT uint64_t SFBDisplayGetConnectionID(SFBDisplayRef d) { return 1; }
EXPORT uint32_t SFBDisplayGetID(SFBDisplayRef d) { return 1; }
EXPORT uint32_t SFBDisplayGetType(SFBDisplayRef d) { return 0; }
EXPORT CFStringRef SFBDisplayGetName(SFBDisplayRef d) { return display_name; }
EXPORT CFStringRef SFBDisplayGetDisplayUID(SFBDisplayRef d) { return display_uid; }
EXPORT CFStringRef SFBDisplayGetMaskPath(SFBDisplayRef d) { return NULL; }
EXPORT uint64_t SFBDisplayGetFlags(SFBDisplayRef d) { return 1; }
EXPORT uint32_t SFBDisplayGetPowerState(SFBDisplayRef d) { return 1; }
EXPORT uint64_t SFBDisplayGetSupportedSurfaceFlags(SFBDisplayRef d) { return 0; }
EXPORT uint32_t SFBDisplayGetSupportedPresentationModes(SFBDisplayRef d) { return 0x7; }
EXPORT uint32_t SFBDisplayGetDotPitch(SFBDisplayRef d) { return 460; }
EXPORT CFIndex SFBDisplayGetMaxLayerCount(SFBDisplayRef d) { return 1; }
EXPORT CFIndex SFBDisplayGetMaxSwapchainCount(SFBDisplayRef d) { return 1; }
EXPORT CGSize SFBDisplayGetDeviceSize(SFBDisplayRef d) { return CGSizeMake(WIDTH, HEIGHT); }
EXPORT CGSize SFBDisplayGetCurrentCanvasSize(SFBDisplayRef d) { return CGSizeMake(WIDTH, HEIGHT); }
EXPORT CFIndex SFBDisplayGetCanvasSizeCount(SFBDisplayRef d) { return 0; }
EXPORT CGSize SFBDisplayGetCanvasSize(SFBDisplayRef d, CFIndex i) { return CGSizeZero; }
EXPORT CFIndex SFBDisplayGetModeCount(SFBDisplayRef d) { return 1; }
EXPORT bool SFBDisplayGetMode(SFBDisplayRef d, CFIndex i, SFBDisplayMode* out) {
	if (i != 0 || !out) return false;
	fill_mode(out);
	return true;
}
EXPORT void SFBDisplayGetPreferredMode(SFBDisplayRef d, SFBDisplayMode* out) { fill_mode(out); }
EXPORT void SFBDisplayGetCurrentMode(SFBDisplayRef d, SFBDisplayMode* out) { fill_mode(out); }
EXPORT bool SFBDisplayGetExtendedProperties(SFBDisplayRef d, void* out) { return false; }
EXPORT CFArrayRef SFBDisplayCopyExtendedPropertyProtocols(SFBDisplayRef d) { return CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks); }
EXPORT bool SFBDisplaySetCurrentMode(SFBDisplayRef d, const SFBDisplayMode* m, CFErrorRef* e) { return true; }
EXPORT bool SFBDisplaySetCurrentUIOrientation(SFBDisplayRef d, uint32_t o, CFErrorRef* e) { return true; }
EXPORT bool SFBDisplaySetCanvasSize(SFBDisplayRef d, CGSize s, CFErrorRef* e) { return true; }
EXPORT bool SFBDisplaySetBacklightState(SFBDisplayRef d, int32_t s, CFErrorRef* e) { return true; }
EXPORT bool SFBDisplaySetBrightnessFactor(SFBDisplayRef d, double f, CFErrorRef* e) { return true; }

EXPORT SFBSwapchainRef SFBDisplayCreateSwapchain(SFBDisplayRef d, uint32_t mode, CFDictionaryRef options, CFErrorRef* error, CGSize size) {
	init_types();
	SFBSwapchainRef sc = (SFBSwapchainRef)_CFRuntimeCreateInstance(NULL, swapchain_type, sizeof(*sc) - sizeof(CFRuntimeBase), NULL);
	sc->width = size.width;
	sc->height = size.height;
	sc->presentation_mode = mode;
	fprintf(stderr, "simfb: swapchain %ux%u mode %u\n", sc->width, sc->height, mode);
	return sc;
}

// --- swapchain

EXPORT CFTypeID SFBSwapchainGetTypeID(void) { init_types(); return swapchain_type; }
EXPORT uint32_t SFBSwapchainGetID(SFBSwapchainRef sc) { return 1; }
EXPORT uint64_t SFBSwapchainGetConnectionID(SFBSwapchainRef sc) { return 1; }
EXPORT uint32_t SFBSwapchainGetDisplayID(SFBSwapchainRef sc) { return 1; }
EXPORT CGSize SFBSwapchainGetFramebufferSize(SFBSwapchainRef sc) { return CGSizeMake(sc->width, sc->height); }
EXPORT uint32_t SFBSwapchainGetPixelFormat(SFBSwapchainRef sc) { return 'BGRA'; }
EXPORT uint32_t SFBSwapchainGetColorspace(SFBSwapchainRef sc) { return 1; }
EXPORT uint32_t SFBSwapchainGetHDRMode(SFBSwapchainRef sc) { return 0; }
EXPORT uint32_t SFBSwapchainGetPresentationMode(SFBSwapchainRef sc) { return sc->presentation_mode; }
EXPORT uint32_t SFBSwapchainGetMaxSurfacesPerOperation(SFBSwapchainRef sc) { return 32; }
EXPORT bool SFBSwapchainAcquireSurfaceFence(SFBSwapchainRef sc, IOSurfaceRef s, void* a, void* b) { return true; }

static void gpu_hook(void);
EXPORT bool SFBSwapchainSwapBegin(SFBSwapchainRef sc, uint64_t t, CFErrorRef* e) {
	gpu_hook();
	if (sc->pending) { CFRelease(sc->pending); sc->pending = NULL; }
	return true;
}
EXPORT bool SFBSwapchainSwapAddSurface(SFBSwapchainRef sc, IOSurfaceRef s, uint64_t a, uint64_t b, CFErrorRef* e, CGRect src, CGRect dst) {
	if (s) {
		if (sc->pending) CFRelease(sc->pending);
		sc->pending = (IOSurfaceRef)CFRetain(s);
	}
	return true;
}
EXPORT void SFBSwapchainSwapSetCallback(SFBSwapchainRef sc, void* context, dispatch_queue_t queue, SFBSwapchainCallback cb) {
	if (sc->cb) Block_release(sc->cb);
	sc->cb = cb ? Block_copy(cb) : NULL;
	sc->cb_context = context;
	sc->cb_queue = queue;
}

// x86Code: frame output. Every completed present is copied into a ring of RING_SLOTS frame slots in a shared-memory
// file (default /Volumes/SystemRoot/dev/shm/x86code-simfb.ring, i.e. Linux's /dev/shm; $SIMFB_RING overrides) that
// the Linux viewers mmap (cli/libexec/fbring.py documents the layout). The slot written is never the latest one; its seq
// is zeroed first and set after the copy, then the header's latest/seq are published (release order). The old
// framebuffer.bgra file ($SIMFB_OUT) is still written, but at most every 2 s (and once more for the last frame), for
// tools that read it. The header also carries present statistics (see struct ring_hdr).
#define RING_SLOTS 4
#define RING_HDR 4096
// x86Code: age_us = publish time - the present's SwapSubmit time (us): a long frame interval with a small age was late
// from CA (the app/render server), a large age was held by the GPU or the refresh pacing (tests/perf.py scroll)
struct ring_slot { _Atomic uint64_t seq; uint64_t time_ns; uint32_t width, height, stride, age_us; };
struct ring_hdr {
	uint32_t magic, version, nslots, slot_bytes, width, height, stride, pad0;
	_Atomic uint64_t seq; _Atomic uint32_t latest; uint32_t pad1;
	uint64_t pad2[2];
	struct ring_slot slots[RING_SLOTS];                    // at 64

	// statistics (at 64 + 32 * RING_SLOTS = 192): counts since the process started
	uint64_t st_submits, st_superseded, st_watchdog, st_copied, st_ready_at_submit;
	uint64_t st_wait_ns, st_wait_max_ns, st_copy_ns, st_copy_max_ns;
	uint64_t st_pid;
};
_Static_assert(__builtin_offsetof(struct ring_hdr, slots) == 64, "ring layout");
_Static_assert(__builtin_offsetof(struct ring_hdr, st_submits) == 192, "ring layout");
static struct ring_hdr* ring;
static size_t ring_slot_bytes;
static uint64_t ring_frames, file_last_ns;
static dispatch_source_t file_timer;

// x86Code: readers on Linux (simview, simstream, fbring.wait) sleep in FUTEX_WAIT on the low 32 bits of the header's
// seq instead of polling; every publish wakes them. Darling processes are Linux processes and its userland makes
// Linux syscalls directly, so this is Linux's futex(2) (shared, not FUTEX_PRIVATE: the page is a /dev/shm file
// mapped by other processes).
static void ring_wake(void* addr) {
	long r;
	__asm__ volatile("syscall" : "=a"(r) : "a"(202L /* SYS_futex */), "D"(addr), "S"(1L /* FUTEX_WAKE */), "d"(0x7fffffffL)
		: "rcx", "r11", "memory");
	(void)r;
}
static uint64_t now_ns(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom; }
static uint64_t wall_ns(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }

static void ring_open(size_t stride, size_t height) {
	static bool tried;
	if (tried) return;
	tried = true;
	const char* path = getenv("SIMFB_RING") ?: "/Volumes/SystemRoot/dev/shm/x86code-simfb.ring";
	ring_slot_bytes = (stride * height + 4095) & ~(size_t)4095;
	size_t len = RING_HDR + RING_SLOTS * ring_slot_bytes;
	char tmp[1100];
	snprintf(tmp, sizeof(tmp), "%s.%d", path, getpid());
	int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);   // a new file per process, renamed into place: readers
	if (fd < 0 || ftruncate(fd, len) != 0) {                   // of an older ring see its inode go away
		fprintf(stderr, "simfb: no frame ring at %s\n", path);
		if (fd >= 0) { close(fd); unlink(tmp); }
		return;
	}
	void* m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) { unlink(tmp); fprintf(stderr, "simfb: frame ring mmap failed\n"); return; }
	ring = m;
	ring->magic = 'RBFS';   // "SFBR" in memory (little-endian)
	ring->version = 2;   // 2: every publish wakes futex waiters on the low word of seq (ring_wake)
	ring->nslots = RING_SLOTS;
	ring->slot_bytes = (uint32_t)ring_slot_bytes;
	ring->st_pid = getpid();
	rename(tmp, path);
	fprintf(stderr, "simfb: frame ring %s (%d slots of %zu bytes)\n", path, RING_SLOTS, ring_slot_bytes);
}

static void file_write(const void* px, uint32_t w, uint32_t h, uint32_t stride, uint64_t seq) {   // on dump_q
	static const char* path;
	static char buf[1024];
	if (!path) {
		path = getenv("SIMFB_OUT");
		if (!path) { snprintf(buf, sizeof(buf), "%s/framebuffer.bgra", getenv("HOME") ?: "/tmp"); path = buf; }
	}
	uint32_t hdr[4] = { w, h, stride, (uint32_t)seq };
	char tmp[1100];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		write(fd, hdr, sizeof(hdr));
		write(fd, px, (size_t)stride * h);
		close(fd);
		rename(tmp, path);
	}
	file_last_ns = now_ns();
}
static uint64_t file_seq;   // ring frame last written to the file
static void file_from_ring(void) {   // on dump_q: the ring's latest slot (only dump_q writes slots)
	uint64_t seq = atomic_load(&ring->seq);
	if (!seq || seq == file_seq) return;
	uint32_t slot = atomic_load(&ring->latest);
	struct ring_slot* rs = &ring->slots[slot];
	file_write((char*)ring + RING_HDR + slot * ring_slot_bytes, rs->width, rs->height, rs->stride, seq);
	file_seq = seq;
}

// present statistics (published in the ring header)
static uint64_t st_submits, st_superseded, st_watchdog, st_ready, st_wait_ns, st_wait_max_ns;

static void dump(SFBSwapchainRef sc, IOSurfaceRef s, uint64_t submit_ns) {   // on dump_q
	uint64_t t0 = now_ns();
	IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
	uint32_t w = (uint32_t)IOSurfaceGetWidth(s), h = (uint32_t)IOSurfaceGetHeight(s), stride = (uint32_t)IOSurfaceGetBytesPerRow(s);
	uint64_t seq = ++sc->seq;
	ring_open(stride, h);
	if (ring && (size_t)stride * h <= ring_slot_bytes) {
		uint32_t slot = (atomic_load_explicit(&ring->latest, memory_order_relaxed) + 1) % RING_SLOTS;
		struct ring_slot* rs = &ring->slots[slot];
		atomic_store_explicit(&rs->seq, 0, memory_order_relaxed);
		atomic_thread_fence(memory_order_release);
		memcpy((char*)ring + RING_HDR + slot * ring_slot_bytes, IOSurfaceGetBaseAddress(s), (size_t)stride * h);
		rs->time_ns = wall_ns(); rs->width = w; rs->height = h; rs->stride = stride;
		{ uint64_t age = submit_ns ? (now_ns() - submit_ns) / 1000 : 0; rs->age_us = age > 0xffffffffu ? 0xffffffffu : (uint32_t)age; }
		ring->width = w; ring->height = h; ring->stride = stride;
		atomic_store_explicit(&rs->seq, ++ring_frames, memory_order_release);
		atomic_store_explicit(&ring->latest, slot, memory_order_release);
		atomic_store_explicit(&ring->seq, ring_frames, memory_order_release);
		ring_wake((void*)&ring->seq);
		IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
		// the file: at most every 2 s, from the ring; the timer writes a newer last frame later
		if (now_ns() - file_last_ns >= 2000000000ull) file_from_ring();
		if (!file_timer) {
			file_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dump_q);
			dispatch_source_set_timer(file_timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
			dispatch_source_set_event_handler(file_timer, ^{ if (now_ns() - file_last_ns >= 1900000000ull) file_from_ring(); });
			dispatch_resume(file_timer);
		}
	} else {   // no ring: every frame to the file, as before
		file_write(IOSurfaceGetBaseAddress(s), w, h, stride, seq);
		IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
	}
	uint64_t dt = now_ns() - t0;
	if (ring) {
		pthread_mutex_lock(&gpu_lock);
		ring->st_submits = st_submits; ring->st_superseded = st_superseded; ring->st_watchdog = st_watchdog;
		ring->st_ready_at_submit = st_ready; ring->st_wait_ns = st_wait_ns; ring->st_wait_max_ns = st_wait_max_ns;
		pthread_mutex_unlock(&gpu_lock);
		ring->st_copied++; ring->st_copy_ns += dt; if (dt > ring->st_copy_max_ns) ring->st_copy_max_ns = dt;
	}
	if (seq == 1 || seq % 600 == 0) fprintf(stderr, "simfb: frame %llu %ux%u\n", (unsigned long long)seq, w, h);
}

// x86Code: GPU completion for presents. With CA's Metal renderer (host/metalhostd, X86CODE_METAL) the GPU may still be
// writing the surface when CA submits it: CA passes no fence to SFBSwapchainSwapAddSurface (both extra arguments are
// 0) and never calls SFBSwapchainAcquireSurfaceFence; on a Mac the host's GPU reads of the surface are ordered after
// the simulator's GPU writes by the driver (implicit IOSurface sync). Here we track it in-process: every
// MTLSimCommandBuffer committed in this process gets a completed handler (fired by MTLSimDriver when metalhostd/mtlvk
// report the GPU finished, after which the surface's shared pages hold the result). A present waits for every
// command buffer committed before SwapSubmit, then the surface is copied. Without Metal (software renderer) nothing
// is outstanding and the copy happens at once. A newer present supersedes an older one still waiting.
static uint64_t gpu_commits;            // command buffers committed so far (their seq: 1..gpu_commits)
static uint64_t gpu_outstanding[4096];  // seqs of committed, not yet completed command buffers (ring of slots)
static size_t gpu_noutstanding;
// x86Code: presents waiting for the GPU are a FIFO of up to PRESENT_QUEUE (CA rotates 3 swapchain surfaces, so a
// surface isn't drawn into again until two newer presents are out). The single waiting slot of before dropped every
// present that came while the previous one still waited (GPU latency ~9 ms vs a 16 ms frame: up to 60% of the
// presents of a home-screen swipe). Only when the queue is full is the oldest dropped ("superseded").
#define PRESENT_QUEUE 2
static struct waiting { SFBSwapchainRef sc; IOSurfaceRef s; uint64_t target, t0, t0ns; } present[PRESENT_QUEUE];
static size_t npresent;
static uint64_t present_ids;   // t0 identifies a waiting present for its watchdog (a counter, unique)
static IMP orig_commit;
static uint64_t stat_waited, stat_immediate, stat_frames;

static bool gpu_done_upto(uint64_t target) {
	for (size_t i = 0; i < gpu_noutstanding; i++) if (gpu_outstanding[i] <= target) return false;
	return true;
}
// x86Code: scanout at the display's refresh. CA's SimDisplay does not wait for the swap callback (finished_update is
// the replaced buffer's seed), so presents came whenever CA rendered: up to ~94 a second while dragging, unevenly
// spaced. A present whose GPU work is done now waits for the next refresh slot after the previous publish (a display
// shows a frame at vsync); a newer ready present replaces one still waiting (counted as superseded). Frames that come
// slower than the refresh are published at once. SIMFB_HZ (default 60) sets the rate, 0 publishes immediately.
static IOSurfaceRef vs_s;
static SFBSwapchainRef vs_sc;
static uint64_t vs_t0ns;
static bool vs_armed;
static uint64_t vs_last_ns, vs_period_ns;
static void vsync_fire(void) {   // on dump_q
	pthread_mutex_lock(&gpu_lock);
	IOSurfaceRef s = vs_s; SFBSwapchainRef sc = vs_sc; uint64_t t0ns = vs_t0ns;
	vs_s = NULL; vs_sc = NULL; vs_armed = false;
	uint64_t now = now_ns();
	// keep the slot grid while frames keep coming; start a new one after a pause
	vs_last_ns = (now - vs_last_ns < 2 * vs_period_ns) ? vs_last_ns + vs_period_ns : now;
	if (vs_last_ns > now) vs_last_ns = now;
	pthread_mutex_unlock(&gpu_lock);
	if (s) { dump(sc, s, t0ns); CFRelease(s); CFRelease(sc); }
}
static void present_fire_locked(size_t i) {   // gpu_lock held: copy waiting present i (and drop it from the queue)
	SFBSwapchainRef sc = present[i].sc;
	IOSurfaceRef s = present[i].s;
	uint64_t t0ns = present[i].t0ns;
	uint64_t w = now_ns() - present[i].t0ns;
	st_wait_ns += w; if (w > st_wait_max_ns) st_wait_max_ns = w;
	memmove(&present[i], &present[i + 1], (npresent - i - 1) * sizeof(present[0]));
	npresent--;
	stat_frames++;
	if (!vs_period_ns) {
		const char* hz = getenv("SIMFB_HZ");
		double f = hz ? atof(hz) : 60;
		vs_period_ns = f > 0 ? (uint64_t)(1e9 / f) : 1;
	}
	if (vs_period_ns <= 1) { dispatch_async(dump_q, ^{ dump(sc, s, t0ns); CFRelease(s); CFRelease(sc); }); return; }
	if (vs_s) { CFRelease(vs_s); CFRelease(vs_sc); st_superseded++; }
	vs_s = s; vs_sc = sc; vs_t0ns = t0ns;
	if (!vs_armed) {
		vs_armed = true;
		uint64_t now = now_ns(), next = vs_last_ns + vs_period_ns;
		if (next <= now) dispatch_async(dump_q, ^{ vsync_fire(); });
		else dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(next - now)), dump_q, ^{ vsync_fire(); });
	}
}
static void present_fire_ready_locked(void) {   // in order: the oldest first, stop at the first not ready
	while (npresent && gpu_done_upto(present[0].target)) present_fire_locked(0);
}
static void gpu_completed(uint64_t seq) {
	pthread_mutex_lock(&gpu_lock);
	for (size_t i = 0; i < gpu_noutstanding; i++)
		if (gpu_outstanding[i] == seq) { gpu_outstanding[i] = gpu_outstanding[--gpu_noutstanding]; break; }
	present_fire_ready_locked();
	pthread_mutex_unlock(&gpu_lock);
}
static void hooked_commit(id self, SEL cmd) {
	pthread_mutex_lock(&gpu_lock);
	uint64_t seq = ++gpu_commits;
	if (gpu_noutstanding < sizeof(gpu_outstanding) / sizeof(gpu_outstanding[0])) gpu_outstanding[gpu_noutstanding++] = seq;
	pthread_mutex_unlock(&gpu_lock);
	void (^h)(id) = ^(id cb) { gpu_completed(seq); };
	((void (*)(id, SEL, void (^)(id)))objc_msgSend)(self, sel_registerName("addCompletedHandler:"), h);
	((void (*)(id, SEL))orig_commit)(self, cmd);
}
static void gpu_hook(void) {   // MTLSimDriver may load after us: retried at every SwapBegin until found
	static bool done;
	if (done) return;
	Class c = objc_getClass("MTLSimCommandBuffer");
	if (!c) return;
	done = true;
	SEL sel = sel_registerName("commit");
	Method m = class_getInstanceMethod(c, sel);   // inherited from Metal's _MTLCommandBuffer
	if (!m) return;
	orig_commit = method_getImplementation(m);
	// override on MTLSimCommandBuffer only (class_addMethod), so other _MTLCommandBuffer subclasses keep theirs
	if (!class_addMethod(c, sel, (IMP)hooked_commit, method_getTypeEncoding(m))) {
		orig_commit = method_setImplementation(class_getInstanceMethod(c, sel), (IMP)hooked_commit);
	}
	fprintf(stderr, "simfb: presents wait for GPU completion of MTLSimCommandBuffers\n");
}
// a present still waiting after 1 s (e.g. mtlvk died and completions never come) is copied anyway, with the older ones
static void present_watchdog(uint64_t id) {
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dump_q, ^{
		pthread_mutex_lock(&gpu_lock);
		for (size_t i = 0; i < npresent; i++) {
			if (present[i].t0 != id) continue;
			fprintf(stderr, "simfb: present waited 1 s for the GPU (%zu command buffers outstanding), copying anyway\n", gpu_noutstanding);
			for (size_t k = 0; k <= i; k++) { st_watchdog++; present_fire_locked(0); }
			break;
		}
		pthread_mutex_unlock(&gpu_lock);
	});
}

EXPORT bool SFBSwapchainSwapSubmit(SFBSwapchainRef sc, CFErrorRef* e) {
	IOSurfaceRef s = sc->pending;
	sc->pending = NULL;
	if (s) {
		static dispatch_once_t once;
		dispatch_once(&once, ^{ dump_q = dispatch_queue_create("simfb.dump", DISPATCH_QUEUE_SERIAL); });
		CFRetain(sc);
		pthread_mutex_lock(&gpu_lock);
		st_submits++;
		if (npresent == PRESENT_QUEUE) {   // full: the oldest is dropped
			CFRelease(present[0].s); CFRelease(present[0].sc); st_superseded++;
			memmove(&present[0], &present[1], (npresent - 1) * sizeof(present[0]));
			npresent--;
		}
		uint64_t id = ++present_ids;
		present[npresent++] = (struct waiting){ sc, s, gpu_commits, id, now_ns() };
		present_fire_ready_locked();
		bool now = npresent == 0;
		if (now) { stat_immediate++; st_ready++; } else stat_waited++;
		if ((stat_immediate + stat_waited) % 600 == 1)
			fprintf(stderr, "simfb: %llu presents waited for the GPU, %llu were ready at submit; %llu copied\n",
				(unsigned long long)stat_waited, (unsigned long long)stat_immediate, (unsigned long long)stat_frames);
		pthread_mutex_unlock(&gpu_lock);
		if (!now) present_watchdog(id);
	}
	SFBSwapchainCallback cb = sc->cb;
	void* ctx = sc->cb_context;
	dispatch_queue_t q = sc->cb_queue ?: dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0);
	sc->cb = NULL;
	if (cb) {
		uint64_t now = mach_absolute_time();
		CFRetain(sc);
		dispatch_async(q, ^{
			cb(NULL, sc, now, now + 16666666, ctx);
			Block_release(cb);
			CFRelease(sc);
		});
	}
	return true;
}
EXPORT bool SFBSwapchainSwapCancel(SFBSwapchainRef sc, CFErrorRef* e) {
	if (sc->pending) { CFRelease(sc->pending); sc->pending = NULL; }
	return true;
}
