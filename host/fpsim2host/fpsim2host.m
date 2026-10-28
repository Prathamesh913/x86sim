// x86Code: stand-in for the Mac host's FileProvider simulator support service, com.apple.FileProvider.sim2host.
//
// In an iOS simulator, FileProvider (fileproviderd, its extensions, apps) sends the file operations a simulator process
// can't do itself to the host's fileproviderd: open by file id, openat/mkdirat/unlinkat/renameat on behalf of the
// extension, xattrs, dataless files... (+[FPDaemonConnection simulatorHostConnection], the NSXPCInterface from
// FPSimulatorSupportInterface()). There is no Mac here: this simulator job (cli/libexec/rundev.sh: com.x86code.fpsim2host,
// MachServices com.apple.FileProvider.sim2host; simshim keeps the connection a simulator one) does them in-process.
// Without it the On My iPhone extension failed "can't resolve did=... [78: Function not implemented]" and Files could
// not open any document.
//
// The calls are handled in -forwardInvocation: with the protocol's own method signatures, so argument types come from
// FileProvider itself. Every method's signature is logged at start (data/var/log/fpsim2host.log), and every call.
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/mount.h>
#include <ftw.h>
#include <stdio.h>

#include <dlfcn.h>
static NSXPCInterface* (*FPSimulatorSupportInterface)(void);   // FileProvider.framework, not in the SDK stubs
extern ssize_t fsgetpath(char* buf, size_t bufsize, fsid_t* fsid, uint64_t objid);
extern int renameatx_np(int fromfd, const char* from, int tofd, const char* to, unsigned int flags);
extern const char* _protocol_getMethodTypeEncoding(Protocol* p, SEL sel, BOOL required, BOOL instance);

static FILE* logf_;
#define LOG(...) do { if (logf_) { fprintf(logf_, __VA_ARGS__); fputc('\n', logf_); fflush(logf_); } } while (0)

static Protocol* proto;

// "@?<v@?@\"NSFileHandle\"@\"NSError\">" -> the block's own type string "v@?@@" (quotes, class names dropped)
static NSString* blockTypes(const char* ext) {
	const char* lt = strchr(ext, '<');
	if (!lt) return nil;
	NSMutableString* out = [NSMutableString string];
	int depth = 0;
	for (const char* p = lt + 1; *p; p++) {
		if (*p == '<') { depth++; continue; }
		if (*p == '>') { if (depth-- == 0) break; continue; }
		if (depth) continue;
		if (*p == '"') { p = strchr(p + 1, '"'); if (!p) break; continue; }
		if (isdigit((unsigned char) *p)) continue;
		[out appendFormat:@"%c", *p];
	}
	return out;
}

// the extended type at index i of a method (0 = return, 1 = self, 2 = _cmd, 3.. = arguments, i.e. NSInvocation index + 1)
static NSString* extArgType(SEL sel, NSUInteger argIndex) {
	const char* e = _protocol_getMethodTypeEncoding(proto, sel, YES, YES);
	if (!e) e = _protocol_getMethodTypeEncoding(proto, sel, NO, YES);
	if (!e) return nil;
	// walk: type, offset, type, offset...
	NSUInteger idx = 0;
	const char* p = e;
	while (*p) {
		const char* start = p;
		// one type: may contain "..." and <...>
		if (*p == '@' && p[1] == '?' && p[2] == '<') {
			int depth = 0;
			p += 2;
			do { if (*p == '<') depth++; else if (*p == '>') depth--; else if (*p == '"') p = strchr(p + 1, '"'); p++; } while (depth && *p);
		} else if (*p == '@' && p[1] == '"') {
			p = strchr(p + 2, '"') + 1;
		} else {
			while (*p && strchr("rnNoORV", *p)) p++;
			if (*p == '^') p++;
			if (*p == '{' || *p == '(' || *p == '[') {
				char open = *p, close = open == '{' ? '}' : open == '(' ? ')' : ']';
				int depth = 0;
				do { if (*p == open) depth++; else if (*p == close) depth--; p++; } while (depth && *p);
			} else if (*p) p++;
		}
		NSString* t = [[NSString alloc] initWithBytes:start length:p - start encoding:NSUTF8StringEncoding];
		while (isdigit((unsigned char) *p)) p++;
		if (idx == argIndex) return t;
		idx++;
	}
	return nil;
}

static NSError* posixError(int e) {
	return [NSError errorWithDomain:NSPOSIXErrorDomain code:e userInfo:nil];
}

static NSFileHandle* handle(int fd) {
	return fd < 0 ? nil : [[NSFileHandle alloc] initWithFileDescriptor:fd closeOnDealloc:YES];
}

static int rmTreeEntry(const char* path, const struct stat* sb, int flag, struct FTW* ftw) {
	return remove(path);
}

@interface NSInvocation (Private)
- (void)invokeUsingIMP:(IMP)imp;
@end

@interface Sim2Host : NSObject
@end

@implementation Sim2Host

- (NSMethodSignature*)methodSignatureForSelector:(SEL)sel {
	struct objc_method_description d = protocol_getMethodDescription(proto, sel, YES, YES);
	if (!d.types) d = protocol_getMethodDescription(proto, sel, NO, YES);
	if (d.types) return [NSMethodSignature signatureWithObjCTypes:d.types];
	return [super methodSignatureForSelector:sel];
}

- (BOOL)respondsToSelector:(SEL)sel {
	if (protocol_getMethodDescription(proto, sel, YES, YES).types || protocol_getMethodDescription(proto, sel, NO, YES).types)
		return YES;
	return [super respondsToSelector:sel];
}

// argument i (2.. = after self/_cmd) as an integer or an object
static long long intArg(NSInvocation* inv, NSUInteger i) {
	const char* t = [inv.methodSignature getArgumentTypeAtIndex:i];
	while (*t && strchr("rnNoORV", *t)) t++;
	NSUInteger size; NSGetSizeAndAlignment(t, &size, NULL);
	long long v = 0;
	if (size <= sizeof(v)) {
		[inv getArgument:&v atIndex:i];
		if (size == 4 && strchr("ilc s", *t)) v = (int) v; // signed 32-bit
		else if (size == 4) v = (unsigned int) v;
		else if (size == 2) v = (short) v;
		else if (size == 1) v = (signed char) v;
	}
	return v;
}
static id objArg(NSInvocation* inv, NSUInteger i) {
	__unsafe_unretained id o = nil;
	[inv getArgument:&o atIndex:i];
	return o;
}

// call the completion block (the last argument) with the given values: objects for '@' slots in order, integers for
// the others; an NSError slot gets err
static void reply(NSInvocation* inv, NSArray* objs, long long num, NSError* err) {
	NSUInteger last = inv.methodSignature.numberOfArguments - 1;
	id block = objArg(inv, last);
	if (!block) return;
	NSString* bt = blockTypes([extArgType(inv.selector, last + 1) UTF8String] ?: "");
	NSString* ext = extArgType(inv.selector, last + 1);
	if (!bt.length) { LOG("  no block type for %s", sel_getName(inv.selector)); return; }
	NSMethodSignature* bs = [NSMethodSignature signatureWithObjCTypes:bt.UTF8String];
	NSInvocation* bi = [NSInvocation invocationWithMethodSignature:bs];
	[bi setTarget:block];
	NSUInteger oi = 0;
	for (NSUInteger a = 1; a < bs.numberOfArguments; a++) {
		const char* t = [bs getArgumentTypeAtIndex:a];
		if (*t == '@') {
			// the last object slot that the extended type calls NSError gets the error
			id v = nil;
			BOOL isErr = a == bs.numberOfArguments - 1 && [ext containsString:@"NSError\">"];
			if (isErr) v = err;
			else if (oi < objs.count) { v = objs[oi] == (id) [NSNull null] ? nil : objs[oi]; oi++; }
			[bi setArgument:&v atIndex:a];
		} else {
			NSUInteger size; NSGetSizeAndAlignment(t, &size, NULL);
			long long v = num;
			[bi setArgument:&v atIndex:a];   // little-endian: the low bytes
		}
	}
	[bi retainArguments];
	struct Block_layout { void* isa; int flags, reserved; void (*invoke)(void); } * bl = (__bridge void*) block;
	[bi invokeUsingIMP:(IMP) bl->invoke];
}

- (void)forwardInvocation:(NSInvocation*)inv {
	SEL sel = inv.selector;
	const char* name = sel_getName(sel);
	NSUInteger n = inv.methodSignature.numberOfArguments;
	NSMutableString* args = [NSMutableString string];
	for (NSUInteger i = 2; i + 1 < n; i++) {
		const char* t = [inv.methodSignature getArgumentTypeAtIndex:i];
		if (*t == '@') [args appendFormat:@" %@", objArg(inv, i)];
		else [args appendFormat:@" %lld", intArg(inv, i)];
	}
	LOG("%s%s", name, args.UTF8String);

	if (!strcmp(name, "sim_requestProtocolVersion:completionHandler:")) {
		// the host's protocol version: answer the one the client speaks
		long long v = intArg(inv, 2);
		reply(inv, @[], v, nil);
		LOG("  -> version %lld", v);
		return;
	}
	if (!strcmp(name, "sim_openByID:device:oflag:completionHandler:")) {
		uint64_t objid = (uint64_t) intArg(inv, 2);
		fsid_t fsid = { { (int32_t) intArg(inv, 3), 0 } };
		int oflag = (int) intArg(inv, 4);
		char path[PATH_MAX];
		ssize_t r = fsgetpath(path, sizeof(path), &fsid, objid);
		if (r < 0) { int e = errno; LOG("  -> fsgetpath errno %d", e); reply(inv, @[[NSNull null]], 0, posixError(e)); return; }
		int fd = open(path, oflag | O_CLOEXEC);
		int e = errno;
		LOG("  -> %s fd %d errno %d", path, fd, fd < 0 ? e : 0);
		reply(inv, @[handle(fd) ?: [NSNull null]], 0, fd < 0 ? posixError(e) : nil);
		return;
	}
	if (!strcmp(name, "sim_openAt:path:flags:permissions:completionHandler:")) {
		NSFileHandle* dir = objArg(inv, 2);
		NSString* path = objArg(inv, 3);
		int fd = openat(dir ? dir.fileDescriptor : AT_FDCWD, path.fileSystemRepresentation, (int) intArg(inv, 4) | O_CLOEXEC, (int) intArg(inv, 5));
		int e = errno;
		LOG("  -> fd %d errno %d", fd, fd < 0 ? e : 0);
		reply(inv, @[handle(fd) ?: [NSNull null]], 0, fd < 0 ? posixError(e) : nil);
		return;
	}
	if (!strcmp(name, "sim_mkdirAt:path:permissions:completionHandler:")) {
		NSFileHandle* dir = objArg(inv, 2);
		NSString* path = objArg(inv, 3);
		int r = mkdirat(dir ? dir.fileDescriptor : AT_FDCWD, path.fileSystemRepresentation, (mode_t) intArg(inv, 4));
		int e = errno;
		LOG("  -> %d errno %d", r, r < 0 ? e : 0);
		reply(inv, @[], 0, r < 0 ? posixError(e) : nil);
		return;
	}
	if (!strcmp(name, "sim_unlinkAt:path:flag:recursive:completionHandler:")) {
		NSFileHandle* dir = objArg(inv, 2);
		NSString* path = objArg(inv, 3);
		int flag = (int) intArg(inv, 4);
		BOOL recursive = intArg(inv, 5) != 0;
		int r;
		if (recursive) {
			char full[PATH_MAX];
			if (dir && path.fileSystemRepresentation[0] != '/') {
				char dpath[PATH_MAX];
				fcntl(dir.fileDescriptor, F_GETPATH, dpath);
				snprintf(full, sizeof(full), "%s/%s", dpath, path.fileSystemRepresentation);
			} else
				snprintf(full, sizeof(full), "%s", path.fileSystemRepresentation);
			r = nftw(full, rmTreeEntry, 32, FTW_DEPTH | FTW_PHYS);
		} else
			r = unlinkat(dir ? dir.fileDescriptor : AT_FDCWD, path.fileSystemRepresentation, flag);
		int e = errno;
		LOG("  -> %d errno %d", r, r < 0 ? e : 0);
		reply(inv, @[], 0, r < 0 ? posixError(e) : nil);
		return;
	}
	if (!strcmp(name, "sim_renameAt:from:tofd:to:flags:completionHandler:")) {
		NSFileHandle* fromDir = objArg(inv, 2);
		NSString* from = objArg(inv, 3);
		NSFileHandle* toDir = objArg(inv, 4);
		NSString* to = objArg(inv, 5);
		int r = renameatx_np(fromDir ? fromDir.fileDescriptor : AT_FDCWD, from.fileSystemRepresentation,
			toDir ? toDir.fileDescriptor : AT_FDCWD, to.fileSystemRepresentation, (unsigned) intArg(inv, 6));
		int e = errno;
		LOG("  -> %d errno %d", r, r < 0 ? e : 0);
		reply(inv, @[], 0, r < 0 ? posixError(e) : nil);
		return;
	}
	if (!strcmp(name, "sim_setOrClearXattr:data:onFile:completionHandler:")) {
		NSString* xname = objArg(inv, 2);
		NSData* data = objArg(inv, 3);
		NSFileHandle* fh = objArg(inv, 4);
		int r = data ? fsetxattr(fh.fileDescriptor, xname.UTF8String, data.bytes, data.length, 0, 0)
			: fremovexattr(fh.fileDescriptor, xname.UTF8String, 0);
		int e = errno;
		if (r < 0 && !data && e == ENOATTR) r = 0;
		LOG("  -> %d errno %d", r, r < 0 ? e : 0);
		reply(inv, @[], 0, r < 0 ? posixError(e) : nil);
		return;
	}
	if (!strcmp(name, "sim_pkgLookup:name:acceptDemotedPkg:detection:completionHandler:")) {
		// (isPackage, isDemoted?, type, error): the host looks for package (bundle) markers; a plain directory or a
		// file isn't one. x86Code: no package detection yet (everything is "not a package").
		reply(inv, @[], 0, nil);
		LOG("  -> not a package");
		return;
	}
	// anything else: not supported here
	LOG("  -> ENOTSUP (not implemented)");
	reply(inv, @[], 0, posixError(ENOTSUP));
}
@end

@interface Delegate : NSObject <NSXPCListenerDelegate>
@end
@implementation Delegate
- (BOOL)listener:(NSXPCListener*)listener shouldAcceptNewConnection:(NSXPCConnection*)c {
	c.exportedInterface = FPSimulatorSupportInterface();
	c.exportedObject = [Sim2Host new];
	LOG("connection from pid %d", c.processIdentifier);
	[c resume];
	return YES;
}
@end

int main(int argc, char** argv) {
	@autoreleasepool {
		const char* home = getenv("HOME");
		char p[1024];
		snprintf(p, sizeof(p), "%s/var/log/fpsim2host.log", home ? home : "/tmp");
		logf_ = fopen(p, "a");
		void* fp = dlopen("/System/Library/Frameworks/FileProvider.framework/FileProvider", RTLD_NOW);
		FPSimulatorSupportInterface = fp ? dlsym(fp, "FPSimulatorSupportInterface") : NULL;
		if (!FPSimulatorSupportInterface) { LOG("fpsim2host: no FPSimulatorSupportInterface (%s)", dlerror()); return 1; }
		NSXPCInterface* iface = FPSimulatorSupportInterface();
		proto = iface.protocol;
		LOG("fpsim2host: protocol %s", proto ? protocol_getName(proto) : "(none)");
		unsigned cnt = 0;
		struct objc_method_description* m = protocol_copyMethodDescriptionList(proto, YES, YES, &cnt);
		for (unsigned i = 0; i < cnt; i++)
			LOG("  %s %s", sel_getName(m[i].name), _protocol_getMethodTypeEncoding(proto, m[i].name, YES, YES) ?: m[i].types);
		free(m);
		static Delegate* d;
		d = [Delegate new];
		// initWithMachServiceName: is marked unavailable on iOS in the SDK; the runtime has it
		NSXPCListener* l = ((id (*)(id, SEL, id)) objc_msgSend)([NSXPCListener alloc], sel_registerName("initWithMachServiceName:"), @"com.apple.FileProvider.sim2host");
		l.delegate = d;
		[l resume];
		[[NSRunLoop mainRunLoop] run];
	}
	return 0;
}
