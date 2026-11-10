// x86Sim: diagnostic shim (Settings / any IconServices client): logs the in-process icon store lookup
// (-[ISConcreteIcon _imageFromStoreForDescriptor:], assessValidationToken:), the LaunchServices database
// sequence number/UUID it compares, and getImageForImageDescriptor:completion: deliveries.
// Processes: ISDIAG_PROGS (comma list) or "Preferences". Log: data/var/log/isdiag.<prog>.<pid>.log
#import <Foundation/Foundation.h>
#include "diagpath.h" // x86Sim: X86SIM_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <objc/runtime.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

static int logfd(void) {
	static int fd = -1;
	if (fd < 0) {
		char p[220];
		snprintf(p, sizeof(p), X86SIM_DATA_DIR "/var/log/isdiag.%s.%d.log", getprogname(), getpid());
		fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	return fd;
}
#define L(...) dprintf(logfd(), __VA_ARGS__)
static double now(void) { return CFAbsoluteTimeGetCurrent(); }
static const char* hex(const void* p, size_t n) {
	static __thread char b[100]; b[0] = 0;
	if (!p) return "(null)";
	for (size_t i = 0; i < n && i < 48; i++) sprintf(b + 2 * i, "%02x", ((const unsigned char*)p)[i]);
	return b;
}

static IMP o_assess, o_store, o_lsseq, o_dataseq, o_get, o_imgfor;
static long d_assess(id self, SEL s, id token) {
	long r = ((long(*)(id, SEL, id))o_assess)(self, s, token);
	static int n; if (n++ < 200) L("%.3f [%p] assessValidationToken icon=%s token(%lu)=%s -> %ld\n", now(), (void*)pthread_self(),
		[[self description] UTF8String], (unsigned long)[token length], [token isKindOfClass:[NSData class]] ? hex([token bytes], [token length]) : [[token description] UTF8String], r);
	return r;
}
static id d_store(id self, SEL s, id desc) {
	id r = ((id(*)(id, SEL, id))o_store)(self, s, desc);
	static int n; if (n++ < 200) L("%.3f [%p] _imageFromStoreForDescriptor icon=%s desc=%s -> %s\n", now(), (void*)pthread_self(), [[self description] UTF8String], [[desc description] UTF8String], [[r description] UTF8String]);
	return r;
}
static id d_imgfor(id self, SEL s, id desc) {
	id r = ((id(*)(id, SEL, id))o_imgfor)(self, s, desc);
	static int n; if (n++ < 200) L("%.3f [%p] imageForDescriptor icon=%s -> %s placeholder=%d\n", now(), (void*)pthread_self(), [[self description] UTF8String], [[r description] UTF8String], [r respondsToSelector:@selector(placeholder)] ? (int)(long)[r performSelector:@selector(placeholder)] : -1);
	return r;
}
static BOOL d_lsseq(id cls, SEL s, unsigned long long* seq, void* uuid) {
	BOOL r = ((BOOL(*)(id, SEL, unsigned long long*, void*))o_lsseq)(cls, s, seq, uuid);
	static int n; if (n++ < 60) L("%.3f +[LSRecord _is_getSequenceNumber:andUUID:] -> %d seq=%llu uuid=%s\n", now(), r, seq ? *seq : 0, hex(uuid, 16));
	return r;
}
static BOOL d_dataseq(id self, SEL s, unsigned long long* seq, void* uuid) {
	BOOL r = ((BOOL(*)(id, SEL, unsigned long long*, void*))o_dataseq)(self, s, seq, uuid);
	static int n; if (n++ < 60) L("%.3f -[NSData _is_getSequenceNumber:andUUID:] -> %d seq=%llu uuid=%s\n", now(), r, seq ? *seq : 0, hex(uuid, 16));
	return r;
}
static void d_get(id self, SEL s, id desc, void (^done)(id)) {
	id me = self;
	double t0 = now();
	L("%.3f [%p] getImageForImageDescriptor icon=%s\n", t0, (void*)pthread_self(), [[me description] UTF8String]);
	static int bt; if (bt++ < 2) L("%s\n", [[[NSThread callStackSymbols] description] UTF8String]);
	void (^w)(id) = done ? ^(id img) {
		L("%.3f [%p] getImageForImageDescriptor DONE (%.2fs) main=%d icon=%s -> %s\n", now(), (void*)pthread_self(), now() - t0, pthread_main_np(), [[me description] UTF8String], [[img description] UTF8String]);
		static int bt2; if (bt2++ < 1) L("%s\n", [[[NSThread callStackSymbols] description] UTF8String]);
		done(img);
		L("%.3f [%p] getImageForImageDescriptor completion returned\n", now(), (void*)pthread_self());
	} : nil;
	((void(*)(id, SEL, id, id))o_get)(self, s, desc, w);
}
// generic: id/BOOL-returning methods with 0/1 object arguments; the return value is logged as an object or a number
#define GN 16
static struct { const char* cls; const char* sel; int meta, args, retobj; IMP orig; } gh[GN] = {
	{ "ISStoreIndex", "validate", 0, 0, 0 },
	{ "ISStoreIndex", "data", 0, 0, 1 },
	{ "ISStore", "unitForUUID:", 0, 1, 1 },
	{ "ISStoreUnit", "storeUnitWithStoreURL:UUID:", 1, 2, 1 },
	{ "ISStoreUnit", "isValid", 0, 0, 0 },
	{ "ISStoreUnit", "remapWithStoreURL:", 0, 1, 0 },
	{ "ISStoreMapTable", "dataForUUID:", 0, 1, 1 },
	{ "ISIconCache", "storeIndex", 0, 0, 1 },
	{ "ISConcreteIcon", "prepareImageForDescriptor:", 0, 1, 1 },
	{ "ISIcon", "prepareImageForDescriptor:", 0, 1, 1 },
	{ "ISIconCache", "store", 0, 0, 1 },
};
static int gfind(id self, SEL s) {
	for (int i = 0; i < GN && gh[i].cls; i++) if (gh[i].orig && sel_isEqual(s, sel_getUid(gh[i].sel))) return i;
	return -1;
}
static const char* dsc(id o) { if (!o) return "nil"; NSString* d = [o description]; return d.length > 300 ? [[d substringToIndex:300] UTF8String] : [d UTF8String]; }
static id g(id self, SEL s, id a, id b) {
	int i = gfind(self, s);
	if (i < 0) return nil;
	id r = ((id(*)(id, SEL, id, id))gh[i].orig)(self, s, a, b);
	static int n; if (n++ < 400) L("%.3f [%p] %c[%s %s] self=%s a=%s b=%s -> %s\n", now(), (void*)pthread_self(), gh[i].meta ? '+' : '-', object_getClassName(self), gh[i].sel,
		gh[i].args || !strcmp(gh[i].sel, "data") ? "" : dsc(self), gh[i].args >= 1 ? dsc(a) : "", gh[i].args >= 2 ? dsc(b) : "",
		gh[i].retobj ? (r ? [[NSString stringWithFormat:@"%s %p%s", object_getClassName(r), r, [r isKindOfClass:[NSData class]] ? [NSString stringWithFormat:@" len %lu", (unsigned long)[r length]].UTF8String : ""] UTF8String] : "nil") : [[NSString stringWithFormat:@"%ld", (long)r] UTF8String]);
	return r;
}
static IMP o_enum;
static void d_enum(id self, SEL s, const unsigned char* uuid, void (^blk)(id, BOOL*)) {
	__block int count = 0;
	void (^w)(id, BOOL*) = ^(id v, BOOL* stop) { count++; blk(v, stop); };
	((void(*)(id, SEL, id, id))o_enum)(self, s, uuid, w);
	static int dumped;
	if (count == 0 && dumped++ < 2) {
		NSData* d = [self performSelector:@selector(data)];
		const unsigned char* b = [d bytes];
		BOOL valid = [d respondsToSelector:sel_getUid("_ISStoreIndex_isValid")] ? (BOOL)(long)[d performSelector:sel_getUid("_ISStoreIndex_isValid")] : -1;
		NSURL* u = [self respondsToSelector:sel_getUid("indexFileURL")] ? [self performSelector:sel_getUid("indexFileURL")] : nil;
		NSData* f = u ? [NSData dataWithContentsOfURL:u] : nil;
		L("   mapped len %lu valid %d head %s\n", (unsigned long)d.length, valid, hex(b, 48));
		L("   file   len %lu        head %s\n", (unsigned long)f.length, hex(f.bytes, 48));
		size_t diff = 0, first = (size_t)-1;
		for (size_t i = 0; i < d.length && i < f.length; i++) if (b[i] != ((const unsigned char*)f.bytes)[i]) { diff++; if (first == (size_t)-1) first = i; }
		L("   mapped vs file: %zu bytes differ, first at %zd\n", diff, (ssize_t)first);
	}
	static int n; if (n++ < 400) L("%.3f [%p] -[ISStoreIndex enumerateValuesForUUID:%s] %d values\n", now(), (void*)pthread_self(), hex(uuid, 16), count);
}
static IMP o_nref;
static unsigned long long d_nref(id self, SEL s, unsigned long long key) {
	unsigned long long r = ((unsigned long long(*)(id, SEL, unsigned long long))o_nref)(self, s, key);
	static int n; if (n++ < 100) {
		const unsigned char* tbl = (const unsigned char*)[self performSelector:sel_getUid("_ISStoreIndex_hashTable")];
		const unsigned char* hdr = (const unsigned char*)[self performSelector:sel_getUid("_ISStoreIndex_hashTableHeader")];
		const unsigned char* b = [self bytes];
		L("   nodeRefForKey %llx (mod 4000 = %llu) -> %llx  self %s %p len %lu bytes %p hdr %p tbl %p hdr=%s tbl[634]=%llx bytes+0x14[634]=%llx\n", key, key % 4000, r,
			object_getClassName(self), self, (unsigned long)[self length], b, hdr, tbl, hex(hdr, 20), tbl ? ((unsigned long long*)tbl)[634] : 0, b ? ((unsigned long long*)(b + 0x14))[634] : 0);
	}
	return r;
}
static void hk(Class c, const char* sel, IMP n, IMP* o, int meta) {
	if (*o || !c) return;
	Method m = meta ? class_getClassMethod(c, sel_getUid(sel)) : class_getInstanceMethod(c, sel_getUid(sel));
	if (m) { *o = method_setImplementation(m, n); L("hooked %c[%s %s] %s\n", meta ? '+' : '-', class_getName(c), sel, method_getTypeEncoding(m)); }
}
static void added(const struct mach_header* mh, intptr_t slide) {
	hk(objc_getClass("ISConcreteIcon"), "assessValidationToken:", (IMP)d_assess, &o_assess, 0);
	hk(objc_getClass("ISConcreteIcon"), "_imageFromStoreForDescriptor:", (IMP)d_store, &o_store, 0);
	hk(objc_getClass("ISConcreteIcon"), "imageForDescriptor:", (IMP)d_imgfor, &o_imgfor, 0);
	hk(objc_getClass("LSRecord"), "_is_getSequenceNumber:andUUID:", (IMP)d_lsseq, &o_lsseq, 1);
	hk(objc_getClass("NSData"), "_is_getSequenceNumber:andUUID:", (IMP)d_dataseq, &o_dataseq, 0);
	hk(objc_getClass("ISConcreteIcon"), "getImageForImageDescriptor:completion:", (IMP)d_get, &o_get, 0);
	hk(objc_getClass("ISStoreIndex"), "enumerateValuesForUUID:bock:", (IMP)d_enum, &o_enum, 0);
	hk(objc_getClass("NSData"), "_ISStoreIndex_nodeRefForKey:", (IMP)d_nref, &o_nref, 0);
	for (int i = 0; i < GN && gh[i].cls; i++) hk(objc_getClass(gh[i].cls), gh[i].sel, (IMP)g, &gh[i].orig, gh[i].meta);
}
__attribute__((constructor)) static void init(void) {
	const char* progs = getenv("ISDIAG_PROGS") ?: "Preferences";
	char b[256]; snprintf(b, sizeof b, ",%s,", progs);
	char me[128]; snprintf(me, sizeof me, ",%s,", getprogname());
	if (!strstr(b, me)) return;
	_dyld_register_func_for_add_image(added);
}
