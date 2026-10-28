// x86Code: diagnostic shim (SpringBoard): logs the icon image update path (data source "did generate image"
// notifications, SBIcon image-did-update callbacks), to find why the first SpringBoard keeps placeholder icons.
// Log: var/log/imgdiag.<pid>.log
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
#include "diagpath.h" // x86Code: X86CODE_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <objc/runtime.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <unistd.h>
#include <execinfo.h>
#include <pthread.h>
#include <dlfcn.h>
static int logfd(void) {
	static int fd = -1;
	if (fd < 0) {
		char p[200];
		snprintf(p, sizeof(p), X86CODE_DATA_DIR "/var/log/imgdiag.%s.%d.log", getprogname(), getpid());
		fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	return fd;
}
#define N 24
static struct { const char* cls; const char* sel; int args; IMP orig; } hooks[N] = {
	{ "SBLeafIcon", "_noteActiveDataSourceDidGenerateImageNotification:", 1 },
	{ "SBIcon", "_notifyImageDidUpdate", 0 },
	{ "SBIconImageView", "iconImageDidUpdate:", 1 },
	{ "SBIconView", "iconImageDidUpdate:", 1 },
	{ "SBIconImageView", "_updateImageAnimated:", 1 },
	{ "SBIconImageView", "updateImageAnimated:", 1 },
	{ "SBApplication", "noteIconDataSourceDidChange", 0 },
	{ "SBHIconImageCache", "_notifyObserversOfUpdateForIcon:", 1 },
	{ "SBHIconImageCache", "iconImageDidUpdate:", 1 },
	{ "SBHIconImageCache", "noteIconImageDidUpdate:", 1 },
	{ "SBIconImageView", "disableImageUpdatesForReason:", 1 },
	{ "SBIcon", "reloadIconImage", 0 },
	{ "SBApplication", "_noteIconDataSourceDidChange", 0 },
	{ "SBIconImageView", "invalidateImageUpdatesAssertion:", 1 },
	{ "SBIconImageView", "setDelayedImageUpdateDueToContentVisibility:", 2 },
	{ "SBIconView", "disableImageUpdatesForReason:", 1 },
};
static int find(id self, SEL s) {
	for (int i = 0; i < N && hooks[i].cls; i++) if (sel_isEqual(s, sel_getUid(hooks[i].sel)) && [self isKindOfClass:objc_getClass(hooks[i].cls)]) return i;
	return -1;
}
static void logcall(id self, SEL s, id a) {
	NSString* d = [NSString stringWithFormat:@"%.3f [%p] -[%s %s] self=%@ arg=%@\n", CFAbsoluteTimeGetCurrent(), (void*)pthread_self(), object_getClassName(self), sel_getName(s), self, (unsigned long)a < 4096 ? [NSString stringWithFormat:@"%lu", (unsigned long)a] : a];
	const char* c = [d UTF8String]; write(logfd(), c, strlen(c));
}
static id h0(id self, SEL s) { int i = find(self, s); logcall(self, s, nil); return i >= 0 ? ((id(*)(id, SEL))hooks[i].orig)(self, s) : nil; }
static id h1(id self, SEL s, id a) { int i = find(self, s); logcall(self, s, a); return i >= 0 ? ((id(*)(id, SEL, id))hooks[i].orig)(self, s, a) : nil; }
static IMP o_bg;
static void d_bg(id self, SEL s, long qos, void (^blk)(void)) {
	static int n;
	NSString* d = [NSString stringWithFormat:@"%.3f [%p] +[SBIcon performBackgroundGeneration qos=%ld] %@\n", CFAbsoluteTimeGetCurrent(), (void*)pthread_self(), qos, n++ < 6 ? [NSThread callStackSymbols] : @""];
	const char* c = [d UTF8String]; write(logfd(), c, strlen(c));
	void (^w)(void) = ^{ dprintf(logfd(), "%.3f bg block start\n", CFAbsoluteTimeGetCurrent()); blk(); dprintf(logfd(), "%.3f bg block end\n", CFAbsoluteTimeGetCurrent()); };
	((void(*)(id, SEL, long, id))o_bg)(self, s, qos, w);
}
typedef struct { double w, h, scale, radius; } info_t;
static IMP o_load;
static void d_load(id self, SEL s, info_t info, id traits, id reason, id ctx, unsigned long opts, long prio, void (^done)(id)) {
	dprintf(logfd(), "%.3f [%p] loadIconLayerInBackground %s icon=%s reason=%s\n", CFAbsoluteTimeGetCurrent(), (void*)pthread_self(), "", [[self description] UTF8String], [[reason description] UTF8String]);
	id me = self;
	void (^w)(id) = done ? ^(id layer){
		dprintf(logfd(), "%.3f [%p] loadIconLayerInBackground DONE icon=%s layer=%s\n", CFAbsoluteTimeGetCurrent(), (void*)pthread_self(), [[me description] UTF8String], [[layer description] UTF8String]);
		NSMutableArray* q = layer ? [NSMutableArray arrayWithObject:layer] : nil; int k = 0;
		while (q.count && k++ < 12) {
			CALayer* l = q[0]; [q removeObjectAtIndex:0];
			id c = l.contents;
			if (c && CFGetTypeID((__bridge CFTypeRef)c) == CGImageGetTypeID()) {
				CGImageRef im = (__bridge CGImageRef)c;
				CFDataRef d = CGDataProviderCopyData(CGImageGetDataProvider(im));
				size_t bpr = CGImageGetBytesPerRow(im), h = CGImageGetHeight(im), w = CGImageGetWidth(im);
				const unsigned char* b = d ? CFDataGetBytePtr(d) : NULL;
				char px[64] = "";
				if (b && CFDataGetLength(d) >= (long)(bpr * h)) { const unsigned char* m = b + (h / 2) * bpr + (w / 2) * (CGImageGetBitsPerPixel(im) / 8); snprintf(px, sizeof px, "%02x%02x%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]); }
				dprintf(logfd(), "   layer %s %p contents CGImage %zux%zu bpc %zu bpp %zu info %x mid %s\n", object_getClassName(l), l, w, h, CGImageGetBitsPerComponent(im), CGImageGetBitsPerPixel(im), CGImageGetBitmapInfo(im), px);
				if (d) CFRelease(d);
			} else dprintf(logfd(), "   layer %s %p contents %s\n", object_getClassName(l), l, c ? object_getClassName(c) : "nil");
			if (l.sublayers) [q addObjectsFromArray:l.sublayers];
		}
		done(layer); } : nil;
	((void(*)(id, SEL, info_t, id, id, id, unsigned long, long, id))o_load)(self, s, info, traits, reason, ctx, opts, prio, w);
}
// x86Code: return values along -[SBIcon makeIconLayerNativelyOrFromImageIfNecessaryWithInfo:...]
#define MK(name) static IMP o_##name; static id d_##name(id self, SEL s, info_t info, id tc, id ctx, unsigned long opts) { \
	id r = ((id(*)(id, SEL, info_t, id, id, unsigned long))o_##name)(self, s, info, tc, ctx, opts); \
	static int n; if (n++ < 60) dprintf(logfd(), "%.3f [%p] -[%s %s] %s opts %lx -> %s\n", CFAbsoluteTimeGetCurrent(), (void*)pthread_self(), object_getClassName(self), sel_getName(s), [[self description] UTF8String], opts, [[r description] UTF8String]); return r; }
MK(lnat) MK(llay) MK(limg) MK(lfi) MK(iimg)
static IMP o_ids; static id d_ids(id cls, SEL s, id ds, id icon, info_t info) { id r = ((id(*)(id, SEL, id, id, info_t))o_ids)(cls, s, ds, icon, info);
	static int n; if (n++ < 40) dprintf(logfd(), "%.3f +iconImageForDataSource %s icon %s -> %s\n", CFAbsoluteTimeGetCurrent(), [[ds description] UTF8String], [[icon description] UTF8String], [[r description] UTF8String]); return r; }
static void hk(const char* c, const char* sel, IMP n, IMP* o) { Method m = class_getInstanceMethod(objc_getClass(c), sel_getUid(sel)); if (m) *o = method_setImplementation(m, n); else dprintf(logfd(), "no %s %s\n", c, sel); }
static IMP o_pause, o_resume, o_fin;
static void d_pause(id self, SEL s) { dprintf(logfd(), "%.3f +[SBHIconImageCache pauseCaching] %s\n", CFAbsoluteTimeGetCurrent(), [[[NSThread callStackSymbols] description] UTF8String]); ((void(*)(id, SEL))o_pause)(self, s); }
static void d_resume(id self, SEL s) { dprintf(logfd(), "%.3f +[SBHIconImageCache resumeCaching] %s\n", CFAbsoluteTimeGetCurrent(), [[[NSThread callStackSymbols] description] UTF8String]); ((void(*)(id, SEL))o_resume)(self, s); }
static void d_fin(id self, SEL s, id icon, id app, id var, id ctx) { dprintf(logfd(), "%.3f finishedAsynchronousLoadOfIcon %s\n", CFAbsoluteTimeGetCurrent(), [[icon description] UTF8String]); ((void(*)(id, SEL, id, id, id, id))o_fin)(self, s, icon, app, var, ctx); }
static void added(const struct mach_header* mh, intptr_t slide) {
	{ Dl_info di; if (dladdr(mh, &di) && strstr(di.dli_fname, "SpringBoardHome.framework/SpringBoardHome")) {
		unsigned long refs[] = { 0x5AE9F8, 0x5A25D0, 0x5B0DF8, 0x5B0E00 };
		for (int i = 0; i < 4; i++) dprintf(logfd(), "selref %lx = %s\n", refs[i], sel_getName(*(SEL*)((char*)mh + refs[i])));
	} }
	Class ic = objc_getClass("SBHIconImageCache");
	if (ic && !o_pause) {
		o_pause = method_setImplementation(class_getClassMethod(ic, sel_getUid("pauseCaching")), (IMP)d_pause);
		o_resume = method_setImplementation(class_getClassMethod(ic, sel_getUid("resumeCaching")), (IMP)d_resume);
		o_fin = method_setImplementation(class_getInstanceMethod(ic, sel_getUid("finishedAsynchronousLoadOfIcon:imageAppearance:variant:context:")), (IMP)d_fin);
	}
	if (!o_load && objc_getClass("SBIcon")) {
		Method m = class_getInstanceMethod(objc_getClass("SBIcon"), sel_getUid("loadIconLayerInBackgroundWithInfo:traitCollection:reason:context:options:priority:completionHandler:"));
		if (m) dprintf(logfd(), "load method %p types %s\n", m, method_getTypeEncoding(m));
		if (m) o_load = method_setImplementation(m, (IMP)d_load);
	}
	if (!o_lnat && objc_getClass("SBLeafIcon")) {
		hk("SBIcon", "makeIconLayerNativelyOrFromImageIfNecessaryWithInfo:traitCollection:context:options:", (IMP)d_lnat, &o_lnat);
		hk("SBLeafIcon", "makeIconLayerWithInfo:traitCollection:context:options:", (IMP)d_llay, &o_llay);
		hk("SBLeafIcon", "makeIconImageWithInfo:traitCollection:context:options:", (IMP)d_limg, &o_limg);
		hk("SBIcon", "makeIconLayerFromImageWithInfo:traitCollection:context:options:", (IMP)d_lfi, &o_lfi);
		hk("SBIcon", "iconImageWithInfo:traitCollection:context:options:", (IMP)d_iimg, &o_iimg);
		Method m = class_getClassMethod(objc_getClass("SBLeafIcon"), sel_getUid("iconImageForDataSource:ofIcon:info:"));
		if (m) o_ids = method_setImplementation(m, (IMP)d_ids);
	}
	if (!o_bg && objc_getClass("SBIcon")) {
		Method m = class_getClassMethod(objc_getClass("SBIcon"), sel_getUid("performBackgroundGenerationActivityWithQOSClass:usingBlock:"));
		if (m) o_bg = method_setImplementation(m, (IMP)d_bg);
	}
	for (int i = 0; i < N && hooks[i].cls; i++) {
		if (hooks[i].orig) continue;
		Class c = objc_getClass(hooks[i].cls);
		if (!c) continue;
		Method m = class_getInstanceMethod(c, sel_getUid(hooks[i].sel));
		if (!m) { hooks[i].orig = (IMP)1; dprintf(logfd(), "no method -[%s %s]\n", hooks[i].cls, hooks[i].sel); continue; }
		hooks[i].orig = method_setImplementation(m, hooks[i].args ? (IMP)h1 : (IMP)h0);
		dprintf(logfd(), "hooked -[%s %s]\n", hooks[i].cls, hooks[i].sel);
	}
}
__attribute__((constructor)) static void init(void) {
	if (strcmp(getprogname(), "SpringBoard")) return;
	if (getenv("IMGDIAG_RELOAD") || !access("/Volumes/SystemRoot/tmp/imgdiag_reload", 0)) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 70 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		id c = [objc_getClass("SBApplicationController") performSelector:sel_getUid("sharedInstance")];
		NSArray* apps = [c performSelector:sel_getUid("allApplications")];
		dprintf(logfd(), "%.3f TEST reload %lu apps\n", CFAbsoluteTimeGetCurrent(), (unsigned long)apps.count);
		for (id a in apps) if ([a respondsToSelector:sel_getUid("_noteIconDataSourceDidChange")]) [a performSelector:sel_getUid("_noteIconDataSourceDidChange")];
	});
	_dyld_register_func_for_add_image(added);
}
// x86Code: who creates CGImages with a NULL colour space ("verify_image_parameters: invalid image colorspace: NULL")?
static CGImageRef my_CGImageCreate(size_t w, size_t h, size_t bpc, size_t bpp, size_t bpr, CGColorSpaceRef cs, CGBitmapInfo bi, CGDataProviderRef dp, const CGFloat* dec, bool interp, CGColorRenderingIntent ri) {
	if (!cs) {
		static int n;
		if (n++ < 6) dprintf(logfd(), "%.3f CGImageCreate NULL cs %zux%zu bpc %zu bpp %zu info %x\n%s\n", CFAbsoluteTimeGetCurrent(), w, h, bpc, bpp, bi, [[[NSThread callStackSymbols] description] UTF8String]);
	}
	return CGImageCreate(w, h, bpc, bpp, bpr, cs, bi, dp, dec, interp, ri);
}
__attribute__((used, section("__DATA,__interpose"))) static struct { const void* n; const void* o; } interp_cgic[] = { { (const void*)my_CGImageCreate, (const void*)CGImageCreate } };
