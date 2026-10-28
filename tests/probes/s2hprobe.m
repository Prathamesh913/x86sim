// x86Code probe: calls host/fpsim2host (com.apple.FileProvider.sim2host) the way FileProvider does, through
// FPSimulatorSupportInterface(): package lookup, directory entries, dataless/materialize, purgeable, sync root,
// materialization handlers.   run-sim.sh tests/probes/s2hprobe <scratch dir inside the device>
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/attr.h>

@protocol S2H
- (void)sim_pkgLookup:(NSFileHandle*)d name:(NSString*)n acceptDemotedPkg:(BOOL)a detection:(unsigned)det completionHandler:(void (^)(BOOL, BOOL, unsigned long long, NSError*))h;
- (void)sim_dirEntryHelper:(NSFileHandle*)d completionHandler:(void (^)(NSData*, unsigned long long, NSError*))h;
- (void)sim_makeDatalessFileAt:(NSFileHandle*)f type:(unsigned)t documentSize:(long long)s entryCount:(unsigned long long)c completionHandler:(void (^)(NSError*))h;
- (void)sim_materializeFile:(NSFileHandle*)f from:(NSFileHandle*)src size:(long long)s completionHandler:(void (^)(NSError*))h;
- (void)sim_unsetDatalessBitAt:(NSFileHandle*)f completionHandler:(void (^)(NSError*))h;
- (void)sim_markPurgeable:(NSFileHandle*)f flags:(unsigned long long)fl syncRoot:(unsigned long long)r completionHandler:(void (^)(NSError*))h;
- (void)sim_setSyncRoot:(NSFileHandle*)f flags:(int)fl completionHandler:(void (^)(NSError*))h;
- (void)sim_registerMaterializationHandler:(id)h forURL:(NSURL*)u completionHandler:(void (^)(NSError*))c;
- (void)sim_unregisterMaterializationHandlerForURL:(NSURL*)u completionHandler:(void (^)(NSError*))c;
@end

int main(int argc, char** argv) {
	@autoreleasepool {
		NSString* dir = [NSString stringWithUTF8String:argv[1]];
		void* fp = dlopen("/System/Library/Frameworks/FileProvider.framework/FileProvider", RTLD_NOW);
		NSXPCInterface* (*iface)(void) = dlsym(fp, "FPSimulatorSupportInterface");
		NSXPCConnection* c = ((id (*)(id, SEL, id, NSUInteger)) objc_msgSend)([NSXPCConnection alloc], sel_registerName("initWithMachServiceName:options:"), @"com.apple.FileProvider.sim2host", 0);
		c.remoteObjectInterface = iface();
		[c resume];
		id<S2H> p = [c synchronousRemoteObjectProxyWithErrorHandler:^(NSError* e) { printf("xpc error %s\n", e.description.UTF8String); }];
		for (NSString* b in @[@"Thing.bundle/Contents", @"Thing.app"]) [[NSFileManager defaultManager] createDirectoryAtPath:[dir stringByAppendingPathComponent:b] withIntermediateDirectories:YES attributes:nil error:nil];
		[@"x" writeToFile:[dir stringByAppendingPathComponent:@"plain.txt"] atomically:NO encoding:NSUTF8StringEncoding error:nil];
		NSFileHandle* dh = [[NSFileHandle alloc] initWithFileDescriptor:open(dir.fileSystemRepresentation, O_RDONLY | O_DIRECTORY) closeOnDealloc:YES];  // NSFileHandle does not open directories
		for (NSString* n in @[@"Thing.bundle", @"Thing.app", @"plain.txt"])
			[p sim_pkgLookup:dh name:n acceptDemotedPkg:NO detection:0 completionHandler:^(BOOL pkg, BOOL dem, unsigned long long t, NSError* e) {
				printf("pkgLookup %s: package %d demoted %d type %llu error %s\n", n.UTF8String, pkg, dem, t, e ? e.description.UTF8String : "-");
			}];
		int dfd = open(dir.fileSystemRepresentation, O_RDONLY | O_DIRECTORY);
		[p sim_dirEntryHelper:[[NSFileHandle alloc] initWithFileDescriptor:dfd closeOnDealloc:YES] completionHandler:^(NSData* d, unsigned long long n, NSError* e) {
			printf("dirEntryHelper: %llu entries, %lu bytes, error %s\n", n, (unsigned long) d.length, e ? e.description.UTF8String : "-");
			const char* b = d.bytes;
			for (unsigned long long i = 0, off = 0; i < n && off < d.length; i++) {
				uint32_t len = *(uint32_t*) (b + off);
				attribute_set_t* rs = (attribute_set_t*) (b + off + 4);
				attrreference_t* nm = (attrreference_t*) (b + off + 4 + sizeof(attribute_set_t));
				printf("  entry %u bytes, returned c=%08x: %s\n", len, rs->commonattr, (char*) nm + nm->attr_dataoffset);
				off += len;
			}
		}];
		NSString* dl = [dir stringByAppendingPathComponent:@"dataless.bin"];
		[[NSData data] writeToFile:dl atomically:NO];
		NSFileHandle* dlh = [NSFileHandle fileHandleForUpdatingAtPath:dl];
		[p sim_makeDatalessFileAt:dlh type:0 documentSize:12345 entryCount:0 completionHandler:^(NSError* e) { printf("makeDataless: %s\n", e ? e.description.UTF8String : "ok"); }];
		struct stat st; stat(dl.fileSystemRepresentation, &st);
		printf("  size %lld flags %#x\n", (long long) st.st_size, st.st_flags);
		NSString* srcp = [dir stringByAppendingPathComponent:@"content.bin"];
		[@"materialized content" writeToFile:srcp atomically:NO encoding:NSUTF8StringEncoding error:nil];
		[p sim_materializeFile:dlh from:[NSFileHandle fileHandleForReadingAtPath:srcp] size:20 completionHandler:^(NSError* e) { printf("materialize: %s\n", e ? e.description.UTF8String : "ok"); }];
		stat(dl.fileSystemRepresentation, &st);
		printf("  size %lld flags %#x content \"%s\"\n", (long long) st.st_size, st.st_flags, [[NSString stringWithContentsOfFile:dl encoding:NSUTF8StringEncoding error:nil] UTF8String]);
		[p sim_makeDatalessFileAt:dlh type:0 documentSize:5 entryCount:0 completionHandler:^(NSError* e) {}];
		[p sim_unsetDatalessBitAt:dlh completionHandler:^(NSError* e) { printf("unsetDataless: %s\n", e ? e.description.UTF8String : "ok"); }];
		stat(dl.fileSystemRepresentation, &st); printf("  flags %#x\n", st.st_flags);
		[p sim_markPurgeable:dlh flags:0x10000 syncRoot:0 completionHandler:^(NSError* e) { printf("markPurgeable: %s\n", e ? e.description.UTF8String : "ok"); }];
		[p sim_setSyncRoot:dh flags:1 completionHandler:^(NSError* e) { printf("setSyncRoot: %s\n", e ? e.description.UTF8String : "ok"); }];
		NSURL* u = [NSURL fileURLWithPath:dir];
		[p sim_registerMaterializationHandler:[NSObject new] forURL:u completionHandler:^(NSError* e) { printf("registerMaterializationHandler: %s\n", e ? e.description.UTF8String : "ok"); }];
		[p sim_unregisterMaterializationHandlerForURL:u completionHandler:^(NSError* e) { printf("unregisterMaterializationHandler: %s\n", e ? e.description.UTF8String : "ok"); }];
	}
	return 0;
}
