// x86Code: diagnostic shim: logs getattrlist-family calls (requested and returned attribute bitmaps) in
// processes named by X86CODE_ATTRDIAG (default lsparse), to see what CoreFoundation's resource keys ask for.
#include "diagpath.h" // x86Code: X86CODE_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <sys/attr.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };
static int on;
// x86Code: without X86CODE_ATTRDIAG (launchd jobs don't get our environment), the names in data/oslog.filter
__attribute__((constructor)) static void init(void) {
	const char* w = getenv("X86CODE_ATTRDIAG");
	if (w) { on = strcmp(getprogname(), w) == 0; return; }
	char f[300] = ""; int fd = open(X86CODE_DATA_DIR "/oslog.filter", O_RDONLY);
	if (fd >= 0) { read(fd, f, sizeof f - 1); close(fd); }
	for (char* t = strtok(f, ",\n"); t; t = strtok(NULL, ",\n")) if (!strcmp(t, getprogname())) on = 1;
}
static void lg(const char* fn, const char* path, int fd, struct attrlist* al, void* buf, size_t sz, unsigned long opts, int r, int e)
{
	if (!on) return;
	uint32_t* w = buf;
	char line[1400];
	int n = snprintf(line, sizeof line, "%s %s fd %d req c=%08x v=%08x d=%08x f=%08x k=%08x opts %lx -> %d errno %d", fn, path ? path : "", fd,
		al->commonattr, al->volattr, al->dirattr, al->fileattr, al->forkattr, opts, r, r < 0 ? e : 0);
	if (r >= 0 && (al->commonattr & ATTR_CMN_RETURNED_ATTRS)) n += snprintf(line + n, sizeof line - n, " len %u ret c=%08x v=%08x d=%08x f=%08x k=%08x", w[0], w[1], w[2], w[3], w[4], w[5]);
	line[n++] = '\n';
	write(2, line, n);
	if (r >= 0 && al->commonattr == 0xd61f9e0d) { n = 0; for (int i = 0; i < 52 && i * 4 < (int) w[0]; i++) n += snprintf(line + n, sizeof line - n, "%08x%c", w[i], i % 8 == 7 ? 10 : 32); line[n++] = 10; write(2, line, n); }
}
static int d_getattrlist(const char* path, struct attrlist* al, void* buf, size_t sz, unsigned int opts)
{ int r = getattrlist(path, al, buf, sz, opts);
	// experiment: add format capability bits (hex in /tmp/fmtcaps) for the request CoreServicesInternal's MountInfoPrepare makes
	if (on && r == 0 && al->commonattr == 0x80000200 && al->volattr == 0xc0062000) {
		char t[20] = ""; int fd = open("/Volumes/SystemRoot/tmp/fmtcaps", O_RDONLY); if (fd >= 0) { read(fd, t, sizeof t - 1); close(fd); }
		uint32_t x = strtoul(t, NULL, 16); ((uint32_t*) buf)[12] |= x; ((uint32_t*) buf)[16] |= x;
	}
	int e = errno; lg("getattrlist", path, -1, al, buf, sz, opts, r, e); errno = e; return r; }
static int d_fgetattrlist(int fd, struct attrlist* al, void* buf, size_t sz, unsigned int opts)
{ int r = fgetattrlist(fd, al, buf, sz, opts); int e = errno; lg("fgetattrlist", NULL, fd, al, buf, sz, opts, r, e); errno = e; return r; }
static int d_getattrlistat(int dfd, const char* path, struct attrlist* al, void* buf, size_t sz, unsigned long opts)
{ int r = getattrlistat(dfd, path, al, buf, sz, opts); int e = errno; lg("getattrlistat", path, dfd, al, buf, sz, opts, r, e); errno = e; return r; }
DYLD_INTERPOSE(d_getattrlist, getattrlist)
DYLD_INTERPOSE(d_fgetattrlist, fgetattrlist)
DYLD_INTERPOSE(d_getattrlistat, getattrlistat)

// x86Code: by-id lookups (document ids: FileProvider/GenerationalStorage resolve "did=" items this way)
#include <sys/mount.h>
extern int openbyid_np(fsid_t* fsid, fsobj_id_t* objid, int flags);
extern ssize_t fsgetpath(char* buf, size_t bufsize, fsid_t* fsid, uint64_t objid);
static void lg2(const char* line) { if (on) write(2, line, strlen(line)); }
static int d_openbyid_np(fsid_t* fsid, fsobj_id_t* objid, int flags)
{ int r = openbyid_np(fsid, objid, flags); int e = errno; char l[200];
	snprintf(l, sizeof l, "openbyid_np fsid %d:%d id %llu flags %#x -> %d errno %d\n", fsid ? fsid->val[0] : -1, fsid ? fsid->val[1] : -1,
		objid ? *(unsigned long long*) objid : 0, flags, r, r < 0 ? e : 0); lg2(l); errno = e; return r; }
static ssize_t d_fsgetpath(char* buf, size_t sz, fsid_t* fsid, uint64_t objid)
{ ssize_t r = fsgetpath(buf, sz, fsid, objid); int e = errno; char l[1300];
	snprintf(l, sizeof l, "fsgetpath fsid %d:%d id %llu -> %zd errno %d %s\n", fsid ? fsid->val[0] : -1, fsid ? fsid->val[1] : -1,
		(unsigned long long) objid, r, r < 0 ? e : 0, r > 0 ? buf : ""); lg2(l); errno = e; return r; }
static int d_open(const char* path, int flags, int mode)
{ int r = open(path, flags, mode); int e = errno;
	if (on && path && !strncmp(path, "/.vol/", 6)) { char l[300]; snprintf(l, sizeof l, "open %s -> %d errno %d\n", path, r, r < 0 ? e : 0); lg2(l); }
	errno = e; return r; }
static int d_openat(int dfd, const char* path, int flags, int mode)
{ int r = openat(dfd, path, flags, mode); int e = errno;
	if (on && path && !strncmp(path, "/.vol/", 6)) { char l[300]; snprintf(l, sizeof l, "openat %s -> %d errno %d\n", path, r, r < 0 ? e : 0); lg2(l); }
	errno = e; return r; }
DYLD_INTERPOSE(d_openbyid_np, openbyid_np)
DYLD_INTERPOSE(d_fsgetpath, fsgetpath)
DYLD_INTERPOSE(d_open, open)
DYLD_INTERPOSE(d_openat, openat)
