// x86Code: diagnostic shim (Speed 27): logs kevent results for sockets (EVFILT_READ/WRITE/SOCK: EOF, errors, socket
// notes), connectx results and failing socket reads/writes, in the processes named in data/oslog.filter, to find where
// Network.framework's "Network is down" (ENETDOWN) on a first connection comes from.
// Log: data/var/log/kevdiag.<prog>.<pid>.log
#include "diagpath.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/stat.h>
#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };

struct kevent_qos_s {
	uint64_t ident; int16_t filter; uint16_t flags; int32_t qos; uint64_t udata;
	uint32_t fflags; uint32_t xflags; int64_t data; uint64_t ext[4];
};
extern int kevent_qos(int, const struct kevent_qos_s*, int, struct kevent_qos_s*, int, void*, size_t*, unsigned int);
extern int kevent_id(uint64_t, const struct kevent_qos_s*, int, struct kevent_qos_s*, int, void*, size_t*, unsigned int);
typedef sa_endpoints_t sae_t;

static int en = -1, lfd = -1;
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
				const char* me = getprogname();
				for (char* t = strtok(buf, ","); t; t = strtok(NULL, ",")) if (!strcmp(t, me) || !strcmp(t, "all")) en = 1;
			}
		}
		if (en) {
			char p[512];
			snprintf(p, sizeof(p), X86CODE_DATA_DIR "/var/log/kevdiag.%s.%d.log", getprogname(), getpid());
			lfd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (lfd < 0) en = 0;
		}
	}
	return en;
}
static void lg(const char* fmt, ...)
{
	char b[600]; struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	int n = snprintf(b, sizeof(b), "%ld.%03ld [%llx] ", (long) (ts.tv_sec % 100000), ts.tv_nsec / 1000000,
		(unsigned long long) pthread_mach_thread_np(pthread_self()));
	va_list ap; va_start(ap, fmt); n += vsnprintf(b + n, sizeof(b) - n, fmt, ap); va_end(ap);
	if (n > (int) sizeof(b) - 2) n = sizeof(b) - 2;
	b[n++] = '\n'; write(lfd, b, n);
}
static int is_sock(uint64_t fd)
{
	struct stat st;
	return fd < 65536 && fstat((int) fd, &st) == 0 && S_ISSOCK(st.st_mode);
}
static void dump(const char* who, const struct kevent_qos_s* ev, int n, int out)
{
	for (int i = 0; i < n; i++) {
		const struct kevent_qos_s* e = &ev[i];
		if (e->filter != EVFILT_READ && e->filter != EVFILT_WRITE && e->filter != -13 /* EVFILT_SOCK */) continue;
		if (!is_sock(e->ident)) continue;
		if (out && e->filter != -13 && !(e->flags & (EV_EOF | EV_ERROR)) && e->fflags == 0) continue;
		struct sockaddr_storage ss; socklen_t sl = sizeof(ss); ss.ss_family = 0;
		getsockname((int) e->ident, (struct sockaddr*) &ss, &sl);
		lg("%s fd %llu af %d filter %d flags 0x%x fflags 0x%x data %lld udata %llx", who, (unsigned long long) e->ident,
			ss.ss_family, e->filter, e->flags, e->fflags, (long long) e->data, (unsigned long long) e->udata);
	}
}
static int my_kevent_qos(int kq, const struct kevent_qos_s* ch, int nch, struct kevent_qos_s* ev, int nev, void* d,
	size_t* da, unsigned int fl)
{
	if (on() && ch && nch > 0) dump("add", ch, nch, 0);
	int r = kevent_qos(kq, ch, nch, ev, nev, d, da, fl);
	if (on() && r > 0 && ev) dump("got", ev, r, 1);
	return r;
}
DYLD_INTERPOSE(my_kevent_qos, kevent_qos)
static int my_kevent_id(uint64_t id, const struct kevent_qos_s* ch, int nch, struct kevent_qos_s* ev, int nev, void* d,
	size_t* da, unsigned int fl)
{
	if (on() && ch && nch > 0) dump("add", ch, nch, 0);
	int r = kevent_id(id, ch, nch, ev, nev, d, da, fl);
	if (on() && r > 0 && ev) dump("got", ev, r, 1);
	return r;
}
DYLD_INTERPOSE(my_kevent_id, kevent_id)
static int my_connectx(int s, const sae_t* ep, uint32_t a, unsigned int fl, const struct iovec* iov, unsigned int n,
	size_t* len, uint32_t* cid)
{
	int r = connectx(s, ep, a, fl, iov, n, len, cid);
	int e = errno;
	if (on()) lg("connectx fd %d srcif %u flags 0x%x iovcnt %u -> %d errno %d len %zu", s, ep ? ep->sae_srcif : 0, fl, n,
		r, r < 0 ? e : 0, len ? *len : 0);
	errno = e;
	return r;
}
DYLD_INTERPOSE(my_connectx, connectx)
#define WRAPRW(ret, name, args, call, fdv) \
	static ret my_##name args { ret r = call; int e = errno; \
		if (r < 0 && e != EAGAIN && on() && is_sock(fdv)) lg(#name " fd %d -> errno %d", (int) fdv, e); \
		errno = e; return r; } \
	DYLD_INTERPOSE(my_##name, name)
WRAPRW(ssize_t, recvmsg, (int s, struct msghdr* m, int f), recvmsg(s, m, f), s)
WRAPRW(ssize_t, sendmsg, (int s, const struct msghdr* m, int f), sendmsg(s, m, f), s)
WRAPRW(ssize_t, readv, (int s, const struct iovec* v, int n), readv(s, v, n), s)
WRAPRW(ssize_t, writev, (int s, const struct iovec* v, int n), writev(s, v, n), s)
WRAPRW(ssize_t, read, (int s, void* b, size_t n), read(s, b, n), s)
WRAPRW(ssize_t, write, (int s, const void* b, size_t n), write(s, b, n), s)
WRAPRW(ssize_t, recvfrom, (int s, void* b, size_t n, int f, struct sockaddr* a, socklen_t* l), recvfrom(s, b, n, f, a, l), s)
WRAPRW(ssize_t, sendto, (int s, const void* b, size_t n, int f, const struct sockaddr* a, socklen_t l), sendto(s, b, n, f, a, l), s)
extern int guarded_close_np(int, const void*);
static int my_close(int f)
{
	if (on() && is_sock(f)) lg("close fd %d", f);
	return close(f);
}
DYLD_INTERPOSE(my_close, close)
static int my_guarded_close_np(int f, const void* g)
{
	if (on() && is_sock(f)) lg("guarded_close fd %d", f);
	return guarded_close_np(f, g);
}
DYLD_INTERPOSE(my_guarded_close_np, guarded_close_np)
