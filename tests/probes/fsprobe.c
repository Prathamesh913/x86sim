// x86Code probe: what FSNode / CFURL / getattrlist report for a bundle path (LaunchServices -43 hunt)
#include "probe_paths.h" // x86Code: X86CODE_RUNTIME_ROOT
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <objc/objc.h>
extern ssize_t fsgetpath(char*, size_t, fsid_t*, uint64_t);
extern int openbyid_np(fsid_t*, fsobj_id_t*, int);
static void dumpcls(const char* n)
{
	Class c = objc_getClass(n);
	printf("class %s %p\n", n, c);
	if (!c || !getenv("DUMP")) return;
	unsigned cnt; Method* m = class_copyMethodList(c, &cnt);
	for (unsigned i = 0; i < cnt; i++) printf("  -%s %s\n", sel_getName(method_getName(m[i])), method_getTypeEncoding(m[i]));
	m = class_copyMethodList(object_getClass((id) c), &cnt);
	for (unsigned i = 0; i < cnt; i++) printf("  +%s %s\n", sel_getName(method_getName(m[i])), method_getTypeEncoding(m[i]));
}
static const char* desc(id o)
{
	if (!o) return "(nil)";
	CFStringRef s = CFCopyDescription((CFTypeRef) o);
	static char b[4][2048]; static int k; k = (k + 1) & 3;
	CFStringGetCString(s, b[k], sizeof b[k], kCFStringEncodingUTF8); CFRelease(s);
	return b[k];
}
int main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : getenv("PROBEPATH");
	if (!path) path = X86CODE_RUNTIME_ROOT "/System/Library/CoreServices/MobileCoreTypes.bundle";
	write(2, "main\n", 5);
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("path %s\n", path);
	char rp[1024]; printf("realpath -> %s (errno %d)\n", realpath(path, rp) ? rp : "NULL", errno);
	struct stat st; int r = stat(path, &st);
	printf("stat r=%d dev=0x%llx ino=%llu mode=0%o flags=0x%x\n", r, (long long) st.st_dev, (unsigned long long) st.st_ino, st.st_mode, st.st_flags);
	struct statfs sf; r = statfs(path, &sf);
	printf("statfs r=%d fsid=%x:%x type=%u fstype=%s mnton=%s from=%s flags=0x%x bsize=%u\n", r, sf.f_fsid.val[0], sf.f_fsid.val[1], sf.f_type, sf.f_fstypename, sf.f_mntonname, sf.f_mntfromname, sf.f_flags, sf.f_bsize);
	// getattrlist with common + fileid etc
	struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT,
		.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_DEVID | ATTR_CMN_FSID | ATTR_CMN_OBJTYPE | ATTR_CMN_OBJID | ATTR_CMN_FILEID | ATTR_CMN_PARENTID | ATTR_CMN_FLAGS };
	struct __attribute__((packed)) { uint32_t len; attribute_set_t ret; dev_t dev; fsid_t fsid; fsobj_type_t type; fsobj_id_t objid; uint32_t flags; uint64_t fileid; uint64_t parentid; } ab;
	memset(&ab, 0, sizeof ab);
	r = getattrlist(path, &al, &ab, sizeof ab, 0);
	printf("getattrlist cmn r=%d errno=%d len=%u ret=0x%x dev=0x%x fsid=%x:%x type=%u objid=%u/%u flags=0x%x fileid=%llu parent=%llu\n", r, r ? errno : 0, ab.len, ab.ret.commonattr, ab.dev, ab.fsid.val[0], ab.fsid.val[1], ab.type, ab.objid.fid_objno, ab.objid.fid_generation, ab.flags, (unsigned long long) ab.fileid, (unsigned long long) ab.parentid);
	if (!r) {
		char fp[1024]; ssize_t n = fsgetpath(fp, sizeof fp, &ab.fsid, ab.fileid);
		printf("fsgetpath(fileid) -> %zd errno=%d %s\n", n, n < 0 ? errno : 0, n > 0 ? fp : "");
		n = fsgetpath(fp, sizeof fp, &ab.fsid, ab.parentid);
		printf("fsgetpath(parent) -> %zd errno=%d %s\n", n, n < 0 ? errno : 0, n > 0 ? fp : "");
		int fd = openbyid_np(&ab.fsid, (fsobj_id_t*) &ab.fileid, O_RDONLY);
		printf("openbyid_np -> %d errno=%d\n", fd, fd < 0 ? errno : 0);
	}
	// FULLPATH, NAME, volume attrs
	struct attrlist al2 = { .bitmapcount = ATTR_BIT_MAP_COUNT, .commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_FULLPATH,
		.forkattr = ATTR_CMNEXT_REALDEVID | ATTR_CMNEXT_REALFSID | ATTR_CMNEXT_NOFIRMLINKPATH | ATTR_CMNEXT_LINKID };
	char big[4096]; memset(big, 0, sizeof big);
	r = getattrlist(path, &al2, big, sizeof big, FSOPT_ATTR_CMN_EXTENDED);
	printf("getattrlist name/fullpath/ext r=%d errno=%d", r, r ? errno : 0);
	if (!r) {
		attribute_set_t* rs = (attribute_set_t*) (big + 4);
		attrreference_t* a1 = (attrreference_t*) (big + 4 + sizeof(attribute_set_t));
		attrreference_t* a2 = a1 + 1;
		printf(" len=%u ret=0x%x/fork 0x%x name=%s full=%s", *(uint32_t*) big, rs->commonattr, rs->forkattr, (char*) a1 + a1->attr_dataoffset, (char*) a2 + a2->attr_dataoffset);
		char* p = (char*) (a2 + 1);
		printf(" realdev=0x%x realfsid=%x:%x", *(uint32_t*) p, ((uint32_t*) (p + 4))[0], ((uint32_t*) (p + 4))[1]);
		attrreference_t* a3 = (attrreference_t*) (p + 12);
		printf(" nofirmlink=%s linkid=%llu", (char*) a3 + a3->attr_dataoffset, *(unsigned long long*) (a3 + 1));
	}
	printf("\n");
	struct attrlist al3 = { .bitmapcount = ATTR_BIT_MAP_COUNT, .commonattr = ATTR_CMN_RETURNED_ATTRS,
		.volattr = ATTR_VOL_INFO | ATTR_VOL_FSTYPE | ATTR_VOL_SIGNATURE | ATTR_VOL_CAPABILITIES | ATTR_VOL_ATTRIBUTES | ATTR_VOL_MOUNTFLAGS };
	memset(big, 0, sizeof big);
	r = getattrlist(path, &al3, big, sizeof big, 0);
	printf("getattrlist vol(on file) r=%d errno=%d\n", r, r ? errno : 0);
	char mnt[1024]; snprintf(mnt, sizeof mnt, "%s", sf.f_mntonname);
	r = getattrlist(mnt, &al3, big, sizeof big, 0);
	printf("getattrlist vol(on mnt %s) r=%d errno=%d len=%u ret=0x%x/vol 0x%x\n", mnt, r, r ? errno : 0, *(uint32_t*) big, ((attribute_set_t*) (big + 4))->commonattr, ((attribute_set_t*) (big + 4))->volattr);
	{
		int fd = open(path, O_RDONLY);
		struct statfs f2; memset(&f2, 0, sizeof f2); int r2 = fstatfs(fd, &f2);
		printf("fstatfs r=%d fsid=%x:%x mnton=%s\n", r2, f2.f_fsid.val[0], f2.f_fsid.val[1], f2.f_mntonname);
		struct attrlist am = { .bitmapcount = ATTR_BIT_MAP_COUNT, .volattr = ATTR_VOL_INFO | ATTR_VOL_MOUNTPOINT | ATTR_VOL_NAME };
		memset(big, 0, sizeof big);
		r2 = getattrlist(path, &am, big, sizeof big, 0);
		attrreference_t* m1 = (attrreference_t*) (big + 4); attrreference_t* m2 = m1 + 1;
		printf("getattrlist VOL_MOUNTPOINT r=%d mnt=%s name=%s\n", r2, (char*) m1 + m1->attr_dataoffset, (char*) m2 + m2->attr_dataoffset);
		r2 = fgetattrlist(fd, &am, big, sizeof big, 0);
		printf("fgetattrlist VOL_MOUNTPOINT r=%d mnt=%s name=%s\n", r2, (char*) m1 + m1->attr_dataoffset, (char*) m2 + m2->attr_dataoffset);
		struct statfs* mt; int nm = getmntinfo(&mt, MNT_NOWAIT);
		for (int i = 0; i < nm; i++) printf("  mnt %x:%x %s %s\n", mt[i].f_fsid.val[0], mt[i].f_fsid.val[1], mt[i].f_fstypename, mt[i].f_mntonname);
		close(fd);
	}
	ssize_t xl = getxattr(path, "com.apple.FinderInfo", NULL, 0, 0, 0);
	printf("getxattr FinderInfo -> %zd errno=%d\n", xl, xl < 0 ? errno : 0);
	char li[4096]; xl = listxattr(path, li, sizeof li, 0);
	printf("listxattr -> %zd errno=%d\n", xl, xl < 0 ? errno : 0);

	// CFURL resource values
	CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*) path, strlen(path), true);
	CFStringRef keys[] = { kCFURLFileResourceIdentifierKey, kCFURLVolumeIdentifierKey, kCFURLIsDirectoryKey, kCFURLIsPackageKey,
		kCFURLCanonicalPathKey, kCFURLVolumeURLKey, kCFURLFileResourceTypeKey, kCFURLIsVolumeKey, kCFURLParentDirectoryURLKey,
		kCFURLVolumeSupportsPersistentIDsKey, kCFURLVolumeIsReadOnlyKey, kCFURLIsAliasFileKey, kCFURLIsSymbolicLinkKey,
		CFSTR("_NSURLVolumeSupportsFileIDsKey"), CFSTR("NSURLFileIdentifierKey"), CFSTR("_NSURLVolumeIsSystemKey"), kCFURLVolumeLocalizedFormatDescriptionKey };
	for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) {
		CFTypeRef v = NULL; CFErrorRef e = NULL;
		Boolean ok = CFURLCopyResourcePropertyForKey(u, keys[i], &v, &e);
		printf("CFURL %s ok=%d v=%s e=%s\n", desc((id) keys[i]), ok, desc((id) v), desc((id) e));
	}
	// FSNode
	printf("dlopen MCS %p %s\n", dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW), dlerror());
	dumpcls("FSNode");
	Class FSNode = objc_getClass("FSNode");
	if (FSNode) {
		for (int flags = 0; flags < 2; flags++) {
			id err = nil;
			id n = ((id(*)(id, SEL, CFURLRef, unsigned long, id*)) objc_msgSend)(((id(*)(id, SEL)) objc_msgSend)((id) FSNode, sel_registerName("alloc")),
				sel_registerName("initWithURL:flags:error:"), u, flags, &err);
			printf("FSNode initWithURL flags=%d -> %s err=%s\n", flags, desc(n), desc(err));
			if (n) {
				const char* sels[] = { "canonicalPathWithError:", "getVolumeIdentifier:error:", "getFileIdentifier:error:", "bundleInfoDictionaryWithError:", "URL", "pathWithError:", "getContentModificationDate:error:", "isDirectory", "isVolume", "isBusyDirectory", "isSideFault", "getOwnerUID:error:", "getInodeNumber:error:", "getDeviceNumber:error:", "isPackage", "isSecuredSystemContent", "getHFSType:creator:error:", "canonical" };
				for (unsigned i = 0; i < sizeof sels / sizeof *sels; i++) {
					SEL s = sel_registerName(sels[i]);
					Method m = class_getInstanceMethod(FSNode, s);
					if (!m) { printf("  %s: n/a\n", sels[i]); continue; }
					unsigned na = method_getNumberOfArguments(m); char rt[32]; method_getReturnType(m, rt, sizeof rt);
					id e = nil; uint64_t a = 0, b = 0;
					if (na == 2 && rt[0] == '@') printf("  %s -> %s\n", sels[i], desc(((id(*)(id, SEL)) objc_msgSend)(n, s)));
					else if (na == 2) printf("  %s -> %d\n", sels[i], ((BOOL(*)(id, SEL)) objc_msgSend)(n, s));
					else if (na == 3 && rt[0] == '@') { id v = ((id(*)(id, SEL, id*)) objc_msgSend)(n, s, &e); printf("  %s -> %s err=%s\n", sels[i], desc(v), desc(e)); }
					else if (na == 4) { BOOL ok = ((BOOL(*)(id, SEL, void*, id*)) objc_msgSend)(n, s, &a, &e); printf("  %s -> %d val=0x%llx err=%s\n", sels[i], ok, (unsigned long long) a, desc(e)); }
					else if (na == 5) { BOOL ok = ((BOOL(*)(id, SEL, void*, void*, id*)) objc_msgSend)(n, s, &a, &b, &e); printf("  %s -> %d val=0x%llx/0x%llx err=%s\n", sels[i], ok, (unsigned long long) a, (unsigned long long) b, desc(e)); }
					else printf("  %s: argc %u ret %s skipped\n", sels[i], na, rt);
				}
			}
		}
	}
	return 0;
}
