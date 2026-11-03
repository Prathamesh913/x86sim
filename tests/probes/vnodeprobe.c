// x86Code probe (fileproviderd vnode): EVFILT_VNODE as XNU does it. Build (Linux): source build/linux-env.sh,
// $MACCC -o tests/probes/vnodeprobe tests/probes/vnodeprobe.c; run: darling shell /path/to/vnodeprobe [dir].
// 1. 1500 vnode knotes in one kqueue all register (XNU has no limit; one inotify instance per knote hit Linux's
//    per-user fs.inotify.max_user_instances, EMFILE -> EV_ERROR EBADF, libdispatch crashed fileproviderd).
// 2. NOTE_WRITE / NOTE_EXTEND / NOTE_ATTRIB / NOTE_RENAME / NOTE_DELETE on a file, NOTE_WRITE on a directory entry.
// 3. Two knotes on one file (two descriptors, one O_EVTONLY) both fire.
// 4. EV_DISPATCH: delivered, so disabled; the file is deleted while disabled; EV_ENABLE reports NOTE_DELETE.
// 5. A deleted file's open descriptor still registers (EV_ADD succeeds, as on XNU).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/resource.h>

static int kq, fails;
static char dir[512];

#define CHECK(c, ...) do { if (c) printf("ok   "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static int reg(int fd, unsigned short flags, unsigned fflags) {
	struct kevent ev, out;
	EV_SET(&ev, fd, EVFILT_VNODE, flags | EV_RECEIPT, fflags, 0, (void *) (long) fd);
	int n = kevent(kq, &ev, 1, &out, 1, NULL);
	static int told;
	if (n < 0) { if (!told++) printf("     kevent(fd %d) = -1 errno %d\n", fd, errno); return errno; }
	if (n == 1 && (out.flags & EV_ERROR) && out.data) {
		if (!told++) {
			struct stat sb; int fs = fstat(fd, &sb), gf = fcntl(fd, F_GETFD);
			struct kevent e2; EV_SET(&e2, fd, EVFILT_VNODE, flags, fflags, 0, 0);
			int r2 = kevent(kq, &e2, 1, NULL, 0, NULL);
			printf("     kevent(fd %d) EV_ERROR %d flags 0x%x filter %d ident %lu; fstat %d getfd %d; retry %d errno %d\n", fd, (int) out.data,
				out.flags, out.filter, (unsigned long) out.ident, fs, gf, r2, r2 < 0 ? errno : 0);
		}
		return (int) out.data;
	}
	return 0;
}
// fflags seen for fd within ms
static unsigned wait_for(int fd, int ms) {
	unsigned got = 0;
	struct timespec ts = { 0, 50 * 1000000 };
	for (int t = 0; t < ms; t += 50) {
		struct kevent ev[8];
		int n = kevent(kq, NULL, 0, ev, 8, &ts);
		for (int i = 0; i < n; i++)
			if ((int) ev[i].ident == fd) got |= ev[i].fflags;
		if (got) { // collect what follows at once too
			n = kevent(kq, NULL, 0, ev, 8, &(struct timespec){ 0, 100 * 1000000 });
			for (int i = 0; i < n; i++)
				if ((int) ev[i].ident == fd) got |= ev[i].fflags;
			break;
		}
	}
	return got;
}
static int mk(const char *name, int flags) {
	char p[700]; snprintf(p, sizeof(p), "%s/%s", dir, name);
	return open(p, flags | O_CREAT, 0644);
}
static void pth(char *p, const char *name) { snprintf(p, 700, "%s/%s", dir, name); }

int main(int argc, char **argv) {
	snprintf(dir, sizeof(dir), "%s/vnodeprobe.%d", argc > 1 ? argv[1] : "/tmp", getpid());
	mkdir(dir, 0755);
	struct rlimit rl = { 4096, 4096 };
	setrlimit(RLIMIT_NOFILE, &rl);
	const unsigned ALL = NOTE_DELETE | NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK | NOTE_RENAME;
	char p[700], q[700];

	// 1. many knotes
	kq = kqueue();
	int N = getenv("VN_N") ? atoi(getenv("VN_N")) : 1500, okc = 0, firsterr = 0, *fds = calloc(N, sizeof(int));
	for (int i = 0; i < N; i++) {
		char name[32]; snprintf(name, sizeof(name), "m%d", i);
		fds[i] = mk(name, O_RDONLY);
		if (fds[i] < 0) { firsterr = firsterr ? firsterr : -errno; continue; }
		int e = reg(fds[i], EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_DELETE);
		if (e == 0) okc++; else if (!firsterr) firsterr = e;
	}
	CHECK(okc == N, "1. %d/%d vnode knotes registered (first error %d)", okc, N, firsterr);
	// the last one still fires
	char lastn[32]; snprintf(lastn, sizeof(lastn), "m%d", N - 1); int w = mk(lastn, O_WRONLY); write(w, "x", 1); close(w);
	CHECK(wait_for(fds[N - 1], 2000) & NOTE_WRITE, "1. knote #%d fires NOTE_WRITE", N);
	for (int i = 0; i < N; i++) { if (fds[i] >= 0) close(fds[i]); char name[32]; snprintf(name, sizeof(name), "m%d", i); pth(p, name); unlink(p); }
	close(kq);

	// 2. file events
	kq = kqueue();
	int f = mk("f", O_RDWR);
	CHECK(reg(f, EV_ADD | EV_CLEAR, ALL) == 0, "2. EV_ADD on a file");
	write(f, "abc", 3);
	unsigned g = wait_for(f, 2000);
	CHECK((g & NOTE_WRITE) && (g & NOTE_EXTEND), "2. write: NOTE_WRITE|NOTE_EXTEND (0x%x)", g);
	pth(p, "f"); chmod(p, 0600);
	g = wait_for(f, 2000); CHECK(g & NOTE_ATTRIB, "2. chmod: NOTE_ATTRIB (0x%x)", g);
	pth(q, "f2"); rename(p, q);
	g = wait_for(f, 2000); CHECK(g & NOTE_RENAME, "2. rename: NOTE_RENAME (0x%x)", g);
	unlink(q);
	g = wait_for(f, 2000); CHECK(g & NOTE_DELETE, "2. unlink: NOTE_DELETE (0x%x)", g);
	close(f);
	// directory entry
	int d = open(dir, O_RDONLY);
	CHECK(reg(d, EV_ADD | EV_CLEAR, NOTE_WRITE) == 0, "2. EV_ADD on a directory");
	int c = mk("child", O_WRONLY); close(c);
	g = wait_for(d, 2000); CHECK(g & NOTE_WRITE, "2. entry created: directory NOTE_WRITE (0x%x)", g);
	close(d);

	// 3. two knotes on one file
	int a = mk("child", O_RDONLY), b = mk("child", O_RDONLY | O_EVTONLY);
	CHECK(reg(a, EV_ADD | EV_CLEAR, NOTE_WRITE) == 0 && reg(b, EV_ADD | EV_CLEAR, NOTE_DELETE | NOTE_WRITE) == 0,
		"3. two knotes (second O_EVTONLY) on one file");
	w = mk("child", O_WRONLY); write(w, "y", 1); close(w);
	{
		unsigned ga = 0, gb = 0; struct kevent ev[8];
		for (int t = 0; t < 20 && !(ga && gb); t++) {
			int n = kevent(kq, NULL, 0, ev, 8, &(struct timespec){ 0, 100 * 1000000 });
			for (int i = 0; i < n; i++) { if ((int) ev[i].ident == a) ga |= ev[i].fflags; if ((int) ev[i].ident == b) gb |= ev[i].fflags; }
		}
		CHECK((ga & NOTE_WRITE) && (gb & NOTE_WRITE), "3. both fire NOTE_WRITE (0x%x 0x%x)", ga, gb);
	}
	reg(a, EV_DELETE, 0); close(a);  // b's watch must survive a's deletion
	pth(p, "child"); unlink(p);
	g = wait_for(b, 2000); CHECK(g & NOTE_DELETE, "3. other knote deleted, unlink: NOTE_DELETE (0x%x)", g);
	close(b);

	// 4. EV_DISPATCH: deleted while disabled
	f = mk("disp", O_RDONLY);
	CHECK(reg(f, EV_ADD | EV_CLEAR | EV_DISPATCH, NOTE_WRITE | NOTE_DELETE) == 0, "4. EV_ADD|EV_DISPATCH");
	w = mk("disp", O_WRONLY); write(w, "z", 1); close(w);
	g = wait_for(f, 2000); CHECK(g & NOTE_WRITE, "4. NOTE_WRITE delivered (knote now disabled) (0x%x)", g);
	pth(p, "disp"); unlink(p);
	g = wait_for(f, 500); CHECK(g == 0, "4. nothing while disabled (0x%x)", g);
	CHECK(reg(f, EV_ENABLE, 0) == 0, "4. EV_ENABLE after the file was deleted");
	g = wait_for(f, 2000); CHECK(g & NOTE_DELETE, "4. NOTE_DELETE on re-enable (0x%x)", g);
	close(f);

	// 5. deleted file
	f = mk("gone", O_RDONLY); pth(p, "gone"); unlink(p);
	CHECK(reg(f, EV_ADD | EV_CLEAR, NOTE_DELETE | NOTE_WRITE) == 0, "5. EV_ADD on a deleted file's descriptor");
	close(f);
	rmdir(dir);
	printf("vnodeprobe: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
