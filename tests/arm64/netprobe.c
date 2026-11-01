// tests/arm64/netprobe.c (Speed 27): Network.framework's first-connection "Network is down" (ENETDOWN) as a probe.
// Network.framework registers a dispatch read source (EVFILT_READ, EV_DISPATCH|EV_UDATA_SPECIFIC) on a new TCP socket
// before connectx(); a hang-up EOF reported then fails the flow. Cases:
//   1 raw:   kqueue + kevent64 EVFILT_READ on a new socket, then connect: what fires
//   2 reuse: a UNIX socket with a dispatch read source gets EOF (peer closed), is closed (source cancelled after),
//            then a TCP socket takes its number, gets a source, connects: does the source see an EOF before connect?
// Usage: netprobe <ipv4> [port]   (prints one line per event; "BAD" lines are the bug)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dispatch/dispatch.h>

static struct sockaddr_in dst;

static int tcp(void)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
	return s;
}

static void raw(void)
{
	int kq = kqueue(), s = tcp();
	struct kevent64_s ev = { .ident = s, .filter = EVFILT_READ, .flags = EV_ADD | EV_ENABLE | EV_DISPATCH | 0x100 /* EV_UDATA_SPECIFIC */, .udata = 1 };
	kevent64(kq, &ev, 1, NULL, 0, 0, NULL);
	struct timespec z = { 0, 50 * 1000 * 1000 };
	int n = kevent64(kq, NULL, 0, &ev, 1, 0, &z);
	printf("raw: before connect: %d events%s flags 0x%x data %lld\n", n, n > 0 ? " BAD" : "", n > 0 ? ev.flags : 0, n > 0 ? ev.data : 0);
	int r = connect(s, (struct sockaddr*) &dst, sizeof(dst));
	printf("raw: connect %d errno %d\n", r, errno);
	n = kevent64(kq, NULL, 0, &ev, 1, 0, &z);
	printf("raw: after connect: %d events%s flags 0x%x\n", n, (n > 0 && (ev.flags & EV_EOF)) ? " BAD" : "", n > 0 ? ev.flags : 0);
	close(s); close(kq);
}

static void reuse(int round)
{
	int sv[2];
	socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	fcntl(sv[0], F_SETFL, O_NONBLOCK);
	dispatch_queue_t q = dispatch_queue_create("p", NULL);
	dispatch_semaphore_t got = dispatch_semaphore_create(0);
	dispatch_source_t a = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, sv[0], 0, q);
	dispatch_source_set_event_handler(a, ^{ dispatch_semaphore_signal(got); dispatch_suspend(a); });
	dispatch_resume(a);
	close(sv[1]); // EOF on sv[0]
	dispatch_semaphore_wait(got, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
	int old = sv[0];
	close(sv[0]); // closed before the source is cancelled (as Network/dnssd do)
	dispatch_source_cancel(a);
	dispatch_resume(a);
	int s = tcp();
	__block int fired = 0;
	dispatch_source_t b = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, s, 0, q);
	dispatch_source_set_event_handler(b, ^{ fired++; dispatch_suspend(b); });
	dispatch_resume(b);
	usleep(100 * 1000);
	printf("reuse %d: unix fd %d, tcp fd %d: read source fired %d time(s) before connect%s\n", round, old, s, fired,
		fired ? " BAD" : "");
	int r = connect(s, (struct sockaddr*) &dst, sizeof(dst));
	(void) r;
	usleep(300 * 1000);
	dispatch_source_cancel(b);
	if (fired) dispatch_resume(b);
	close(s);
}

int main(int argc, char** argv)
{
	dst.sin_family = AF_INET;
	dst.sin_port = htons(argc > 2 ? atoi(argv[2]) : 443);
	inet_pton(AF_INET, argc > 1 ? argv[1] : "17.253.23.206", &dst.sin_addr);
	setvbuf(stdout, NULL, _IONBF, 0);
	raw();
	for (int i = 0; i < 5; i++) reuse(i);
	return 0;
}
