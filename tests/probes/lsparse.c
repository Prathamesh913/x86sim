// x86Code: call LaunchServices' private _LSParseLoadCommands on an executable and print what it records
// (LSExecutablePlatformKey is 0 in every lsd record on Darling, 7 on the Mac). Usage: lsparse <exec>
#include "probe_paths.h" // x86Code: X86CODE_RUNTIME_ROOT
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <objc/runtime.h>
#include <objc/message.h>
extern int dyld_get_active_platform(void);
extern const CFStringRef _kCFURLFileIDKey;
#include <sys/attr.h>
extern void dyld_get_image_versions(const void* mh, void (^cb)(uint32_t, uint32_t, uint32_t));
typedef void (*parse_t)(int fd, off_t size, CFMutableDictionaryRef d, void* slices, bool* weird);
int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	const char* path = argc > 1 ? argv[1] : X86CODE_RUNTIME_ROOT "/Applications/News.app/PlugIns/NewsTag.appex/NewsTag";
	printf("start\n");
	void* h = dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
	printf("dlopen %p %s\n", h, h ? "" : dlerror());
	char* anchor = dlsym(h, "CSIdentityAuthorityGetTypeID");
	parse_t parse = (parse_t) (anchor + (0xff7ef - 0x1ba41a)); // offsets in the 26.5 x86_64 slice
	printf("active %d anchor %p\n", dyld_get_active_platform(), anchor);
	int fd = open(path, O_RDONLY); struct stat st; fstat(fd, &st);
	printf("fd %d size %lld\n", fd, (long long) st.st_size);
	uint32_t fh[2 + 5 * 4]; pread(fd, fh, sizeof fh, 0);
	off_t off = 0; if (fh[0] == 0xbebafeca) off = __builtin_bswap32(fh[2 + 2]);
	uint32_t mh[7]; ssize_t n = pread(fd, mh, 28, off);
	printf("slice off %lld pread %zd magic %x ncmds %u sizeofcmds %u\n", (long long) off, n, mh[0], mh[4], mh[5]);
	size_t sz = 32 + mh[5]; char* buf = malloc(sz); n = pread(fd, buf, sz, off);
	printf("pread2 %zd/%zu magic %x\n", n, sz, *(uint32_t*) buf);
	dyld_get_image_versions(buf, ^(uint32_t p, uint32_t s, uint32_t m) { printf("  manual: platform %u sdk %x minos %x\n", p, s, m); });
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	// the caller (_LSAddExecutableFormatInfo) gets the size from -[FSNode getLength:error:] and skips parsing if 0
	CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*) path, strlen(path), false);
	const CFStringRef keys[] = { kCFURLFileSizeKey, kCFURLTotalFileSizeKey, kCFURLFileAllocatedSizeKey, _kCFURLFileIDKey, kCFURLFileResourceIdentifierKey };
	for (int i = 0; i < 5; i++) { CFTypeRef v = NULL; CFErrorRef e = NULL; Boolean ok = CFURLCopyResourcePropertyForKey(u, keys[i], &v, &e); printf("CFURL key %d ok %d: ", i, ok); if (v) CFShow(v); else printf("\n"); if (e) CFShow(e); }
	id node = ((id (*)(id, SEL, CFURLRef, unsigned long, void*)) objc_msgSend)(((id (*)(id, SEL)) objc_msgSend)((id) objc_getClass("FSNode"), sel_getUid("alloc")), sel_getUid("initWithURL:flags:error:"), u, 0, NULL);
	for (unsigned long fl = 1; fl <= 8; fl <<= 1) { void* e1 = NULL;
		id n1 = ((id (*)(id, SEL, CFURLRef, unsigned long, void**)) objc_msgSend)(((id (*)(id, SEL)) objc_msgSend)((id) objc_getClass("FSNode"), sel_getUid("alloc")), sel_getUid("initWithURL:flags:error:"), u, fl, &e1);
		printf("FSNode flags %lu -> %p\n", fl, n1); if (e1) CFShow(e1); }
	long long len = -1; void* err = NULL;
	BOOL ok = ((BOOL (*)(id, SEL, long long*, void**)) objc_msgSend)(node, sel_getUid("getLength:error:"), &len, &err);
	printf("FSNode %p getLength ok %d len %lld err %p\n", node, ok, len, err); if (err) CFShow(err);
	{ // what CF's file ID comes from: getattrlist ATTR_CMN_FILEID / ATTR_CMN_OBJID
		struct attrlist al = { ATTR_BIT_MAP_COUNT, 0, ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_OBJTYPE | ATTR_CMN_FSID | ATTR_CMN_OBJID | ATTR_CMN_FILEID, 0, 0, 0, 0 };
		char buf[256]; int r = getattrlist(path, &al, buf, sizeof buf, 0);
		printf("getattrlist -> %d errno %d len %u\n", r, r ? errno : 0, *(uint32_t*) buf);
		if (!r) { uint32_t* w = (uint32_t*) buf; printf("  returned %08x %08x %08x %08x %08x; words:", w[1], w[2], w[3], w[4], w[5]); for (int i = 6; i < 16; i++) printf(" %08x", w[i]); printf("\n"); }
		struct stat st2; stat(path, &st2); printf("  st_ino %llx st_dev %x\n", (unsigned long long) st2.st_ino, st2.st_dev);
	}
	bool weird = 0;
	parse(fd, st.st_size, d, NULL, &weird);
	CFShow(d); printf("weird %d\n", weird);
	return 0;
}
