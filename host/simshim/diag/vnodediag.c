// x86Code: diagnostic shim: logs every dispatch vnode source (fd, mask, the thread's last failed open()/openat()),
// and the caller's backtrace for one on a bad descriptor (fd < 0); and every EVFILT_VNODE EV_ADD that reaches
// kevent_qos/kevent_id with a bad ident, with a backtrace. Log: <device data>/var/log/vnodediag.<pid>.log
#include "diagpath.h" // x86Code: X86CODE_DATA_DIR (the device data directory, set by build/mac-build-diag.sh)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <execinfo.h>
#include <dispatch/dispatch.h>
#include <sys/event.h>
struct kevent_qos_s { uint64_t ident; int16_t filter; uint16_t flags; int32_t qos; uint64_t udata; uint32_t fflags; uint32_t xflags; int64_t data; uint64_t ext[4]; };
extern int kevent_qos(int, const struct kevent_qos_s*, int, struct kevent_qos_s*, int, void*, size_t*, unsigned int);
extern int kevent_id(uint64_t, const struct kevent_qos_s*, int, struct kevent_qos_s*, int, void*, size_t*, unsigned int);
#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };
static __thread char lastfail[1100];
static int logfd(void)
{
	static int fd = -1;
	if (fd < 0) {
		char p[1024];
		snprintf(p, sizeof p, X86CODE_DATA_DIR "/var/log/vnodediag.%d.log", getpid());
		fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	return fd;
}
static int my_open(const char* path, int flags, ...)
{
	va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
	int r = open(path, flags, mode);
	if (r < 0) { int e = errno; snprintf(lastfail, sizeof lastfail, "open(%s, 0x%x) errno %d", path, flags, e); errno = e; }
	return r;
}
static int my_openat(int dfd, const char* path, int flags, ...)
{
	va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
	int r = openat(dfd, path, flags, mode);
	if (r < 0) { int e = errno; snprintf(lastfail, sizeof lastfail, "openat(%d, %s, 0x%x) errno %d", dfd, path, flags, e); errno = e; }
	return r;
}
static dispatch_source_t my_dispatch_source_create(dispatch_source_type_t t, uintptr_t h, uintptr_t m, dispatch_queue_t q)
{
	if (t == DISPATCH_SOURCE_TYPE_VNODE) {
		int fd = logfd(); char b[1300];
		int n = snprintf(b, sizeof b, "vnode source on fd %d mask 0x%lx; last failed: %s\n", (int)h, (unsigned long)m, lastfail);
		write(fd, b, n);
		if ((int)h >= 0) return dispatch_source_create(t, h, m, q);
		void* bt[40]; int k = backtrace(bt, 40); backtrace_symbols_fd(bt, k, fd); write(fd, "\n", 1);
	}
	return dispatch_source_create(t, h, m, q);
}
// x86Code: libdispatch registers a source's knote synchronously from dispatch_resume/dispatch_activate on the calling
// thread, so a backtrace here shows who resumed a vnode source on a bad descriptor
static void check(const char* fn, const struct kevent_qos_s* c, int n)
{
	for (int i = 0; i < n; i++) {
		if (c[i].filter != EVFILT_VNODE || !(c[i].flags & EV_ADD) || (int64_t)c[i].ident >= 0 && c[i].ident <= 0x7fffffff) continue;
		int fd = logfd(); char b[1300];
		int k = snprintf(b, sizeof b, "%s: EVFILT_VNODE EV_ADD ident 0x%llx udata 0x%llx; last failed: %s\n", fn,
			(unsigned long long)c[i].ident, (unsigned long long)c[i].udata, lastfail);
		write(fd, b, k);
		void* bt[60]; int m = backtrace(bt, 60); backtrace_symbols_fd(bt, m, fd); write(fd, "\n", 1);
	}
}
static int my_kevent_qos(int kq, const struct kevent_qos_s* c, int nc, struct kevent_qos_s* e, int ne, void* d, size_t* da, unsigned int f)
{
	check("kevent_qos", c, nc);
	return kevent_qos(kq, c, nc, e, ne, d, da, f);
}
static int my_kevent_id(uint64_t id, const struct kevent_qos_s* c, int nc, struct kevent_qos_s* e, int ne, void* d, size_t* da, unsigned int f)
{
	check("kevent_id", c, nc);
	return kevent_id(id, c, nc, e, ne, d, da, f);
}
DYLD_INTERPOSE(my_kevent_qos, kevent_qos)
DYLD_INTERPOSE(my_kevent_id, kevent_id)
DYLD_INTERPOSE(my_open, open)
DYLD_INTERPOSE(my_openat, openat)
DYLD_INTERPOSE(my_dispatch_source_create, dispatch_source_create)
