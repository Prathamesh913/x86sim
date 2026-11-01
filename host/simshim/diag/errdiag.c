// x86Code: diagnostic shim: logs file-system calls failing with EINVAL/ENOTSUP/ENOSYS (and their callers) in the
// processes named in data/oslog.filter. Log: data/var/log/errdiag.<prog>.<pid>.log
#include "diagpath.h" // x86Code: X86CODE_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/attr.h>
#include <sys/xattr.h>
#include <sys/mount.h>
extern int fsctl(const char*, unsigned long, void*, unsigned int);
extern int ffsctl(int, unsigned long, void*, unsigned int);
extern int openbyid_np(fsid_t*, fsobj_id_t*, int);
extern ssize_t fsgetpath(char*, size_t, fsid_t*, uint64_t);
#include <sys/clonefile.h>
#include <copyfile.h>
#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };
static int en = -1, fd = -1, all_enoent;
static int on(void)
{
	if (en < 0) {
		char buf[1024] = {0};
		int f = open(X86CODE_DATA_DIR "/oslog.filter", O_RDONLY);
		en = 0;
		if (f >= 0) {
			ssize_t n = read(f, buf, sizeof(buf) - 1); close(f);
			if (n > 0) {
				buf[strcspn(buf, "\n")] = 0;
				for (char* t = strtok(buf, ","); t; t = strtok(NULL, ",")) { if (!strcmp(t, getprogname()) || !strcmp(t, "all")) en = 1; if (!strcmp(t, "+enoent")) all_enoent = 1; }
			}
		}
		if (en) {
			char p[300];
			snprintf(p, sizeof(p), X86CODE_DATA_DIR "/var/log/errdiag.%s.%d.log", getprogname(), getpid());
			fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (fd < 0) en = 0;
		}
	}
	return en;
}
static void report(const char* what, const char* path, long arg, int err)
{
	if (!on()) return;
	// Speed 27: ENOENT too for renames (lsd's atomic CSStore move: "file doesn't exist")
	if (err != EINVAL && err != ENOTSUP && err != ENOSYS && err != ENOTTY && !(err == ENOENT && (all_enoent || strstr(what, "rename"))))
		return;
	dprintf(fd, "%s(%s, 0x%lx) -> errno %d\n", what, path ? path : "-", arg, err);
	void* f[10]; int k = backtrace(f, 10);
	for (int i = 2; i < k; i++) { Dl_info di; if (dladdr(f[i], &di) && di.dli_fname) dprintf(fd, "    %s %s+0x%lx\n", strrchr(di.dli_fname, '/') + 1, di.dli_sname ? di.dli_sname : "", (long) ((char*) f[i] - (char*) di.dli_fbase)); }
}
#define WRAP(ret, name, params, args, pathexpr, argexpr) \
	static ret d_##name params { ret r = name args; if (r == (ret) -1) { int e = errno; report(#name, pathexpr, (long) (argexpr), e); errno = e; } return r; } \
	DYLD_INTERPOSE(d_##name, name)
WRAP(int, mkdir, (const char* p, mode_t m), (p, m), p, m)
WRAP(int, mkdirat, (int d, const char* p, mode_t m), (d, p, m), p, m)
WRAP(int, rename, (const char* a, const char* b), (a, b), a, 0)
WRAP(int, renameat, (int fa, const char* a, int fb, const char* b), (fa, a, fb, b), a, 0)
WRAP(int, renamex_np, (const char* a, const char* b, unsigned f), (a, b, f), a, f)
WRAP(int, renameatx_np, (int fa, const char* a, int fb, const char* b, unsigned f), (fa, a, fb, b, f), a, f)
WRAP(int, setattrlist, (const char* p, void* l, void* b, size_t s, unsigned int o), (p, l, b, s, o), p, ((struct attrlist*) l)->commonattr)
WRAP(int, fsetattrlist, (int f, void* l, void* b, size_t s, unsigned int o), (f, l, b, s, o), NULL, ((struct attrlist*) l)->commonattr)
WRAP(int, getattrlist, (const char* p, void* l, void* b, size_t s, unsigned int o), (p, l, b, s, o), p, ((struct attrlist*) l)->commonattr)
WRAP(int, fgetattrlist, (int f, void* l, void* b, size_t s, unsigned int o), (f, l, b, s, o), NULL, ((struct attrlist*) l)->commonattr)
WRAP(int, fsctl, (const char* p, unsigned long c, void* d, unsigned int o), (p, c, d, o), p, c)
WRAP(int, ffsctl, (int f, unsigned long c, void* d, unsigned int o), (f, c, d, o), NULL, c)
WRAP(int, chflags, (const char* p, unsigned int fl), (p, fl), p, fl)
WRAP(int, fchflags, (int f, unsigned int fl), (f, fl), NULL, fl)
WRAP(int, setxattr, (const char* p, const char* n, const void* v, size_t s, u_int32_t pos, int o), (p, n, v, s, pos, o), n, o)
WRAP(int, fsetxattr, (int f, const char* n, const void* v, size_t s, u_int32_t pos, int o), (f, n, v, s, pos, o), n, o)
WRAP(int, clonefile, (const char* a, const char* b, uint32_t f), (a, b, f), a, f)
WRAP(int, chmod, (const char* p, mode_t m), (p, m), p, m)
WRAP(int, fchmod, (int f, mode_t m), (f, m), NULL, m)
WRAP(int, link, (const char* a, const char* b), (a, b), a, 0)
WRAP(int, symlink, (const char* a, const char* b), (a, b), b, 0)
WRAP(int, unlink, (const char* p), (p), p, 0)
WRAP(int, rmdir, (const char* p), (p), p, 0)
WRAP(int, copyfile, (const char* a, const char* b, copyfile_state_t s, copyfile_flags_t f), (a, b, s, f), a, f)
WRAP(int, fcopyfile, (int a, int b, copyfile_state_t s, copyfile_flags_t f), (a, b, s, f), NULL, f)
WRAP(int, ftruncate, (int f, off_t l), (f, l), NULL, l)
WRAP(int, fsync, (int f), (f), NULL, 0)
// Speed 27: writes and mappings (Core ML's BNNS graph compiler: "failed to write out file ... Invalid argument")
WRAP(ssize_t, write, (int f, const void* b, size_t n), (f, b, n), NULL, n)
WRAP(ssize_t, pwrite, (int f, const void* b, size_t n, off_t o), (f, b, n, o), NULL, o)
WRAP(ssize_t, writev, (int f, const struct iovec* v, int n), (f, v, n), NULL, n)
WRAP(void*, mmap, (void* a, size_t l, int p, int fl, int f, off_t o), (a, l, p, fl, f, o), NULL, fl)
WRAP(int, msync, (void* a, size_t l, int fl), (a, l, fl), NULL, fl)
WRAP(int, madvise, (void* a, size_t l, int adv), (a, l, adv), NULL, adv)
WRAP(int, fsgetpath, (char* b, size_t s, fsid_t* fs, uint64_t id), (b, s, fs, id), NULL, (long) id)
WRAP(int, openbyid_np, (fsid_t* fs, fsobj_id_t* id, int fl), (fs, id, fl), NULL, fl)
WRAP(int, getattrlistbulk, (int d, void* l, void* b, size_t s, uint64_t o), (d, l, b, s, o), NULL, ((struct attrlist*) l)->commonattr)
WRAP(int, getattrlistat, (int d, const char* p, void* l, void* b, size_t s, unsigned long o), (d, p, l, b, s, o), p, ((struct attrlist*) l)->commonattr)
WRAP(int, statfs, (const char* p, struct statfs* b), (p, b), p, 0)
WRAP(int, fstatfs, (int f, struct statfs* b), (f, b), NULL, 0)
WRAP(off_t, lseek, (int f, off_t o, int w), (f, o, w), NULL, w)
static int d_fcntl(int f, int c, ...)
{
	va_list ap; va_start(ap, c); void* a = va_arg(ap, void*); va_end(ap);
	int r = fcntl(f, c, a);
	if (r == -1) { int e = errno; report("fcntl", NULL, c, e); errno = e; }
	return r;
}
DYLD_INTERPOSE(d_fcntl, fcntl)
static int d_open(const char* p, int fl, ...)
{
	va_list ap; va_start(ap, fl); int m = va_arg(ap, int); va_end(ap);
	int r = open(p, fl, m);
	if (r == -1) { int e = errno; report("open", p, fl, e); errno = e; }
	return r;
}
DYLD_INTERPOSE(d_open, open)
static int d_openat(int d, const char* p, int fl, ...)
{
	va_list ap; va_start(ap, fl); int m = va_arg(ap, int); va_end(ap);
	int r = openat(d, p, fl, m);
	if (r == -1) { int e = errno; report("openat", p, fl, e); errno = e; }
	return r;
}
DYLD_INTERPOSE(d_openat, openat)
