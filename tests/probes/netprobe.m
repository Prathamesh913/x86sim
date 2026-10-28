// x86Code: networking probe for the simulator (Spike 3.8). Run as a job (cli/libexec/rundev.sh com.x86code.netprobe=tests/probes/netprobe).
// Checks, in order: getaddrinfo, DNSServiceGetAddrInfo, PF_ROUTE RTM_GET, SCNetworkReachability, nw_path_monitor, NSURLSession GET.
#import <Foundation/Foundation.h>
#import <Network/Network.h>
#import <SystemConfiguration/SystemConfiguration.h>
#include <dns_sd.h>
#include <netdb.h>
#include <arpa/inet.h>
// net/route.h is not in the iOS SDK: XNU's rt_msghdr
struct rt_metrics { uint32_t rmx_locks, rmx_mtu, rmx_hopcount; int32_t rmx_expire; uint32_t rmx_recvpipe, rmx_sendpipe, rmx_ssthresh, rmx_rtt, rmx_rttvar, rmx_pksent, rmx_state, rmx_filler[3]; };
struct rt_msghdr { u_short rtm_msglen; u_char rtm_version, rtm_type; u_short rtm_index; int rtm_flags, rtm_addrs; pid_t rtm_pid; int rtm_seq, rtm_errno, rtm_use; uint32_t rtm_inits; struct rt_metrics rtm_rmx; };
#define RTM_VERSION 5
#define RTM_GET 4
#define RTA_DST 1
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/event.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#ifndef IP_BOUND_IF
#define IP_BOUND_IF 25
#endif
#ifndef EVFILT_SOCK
#define EVFILT_SOCK (-13)
#endif

// The steps of Network.framework's TCP connect, one by one, with each step's errno
static void rawconnect(const char *ip, int ifindex) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	printf("raw: socket %d; NOSIGPIPE %d", fd, setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one));
	int r = setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &ifindex, sizeof ifindex);
	printf("; IP_BOUND_IF(%d) %d errno %d", ifindex, r, r ? errno : 0);
	fcntl(fd, F_SETFL, O_NONBLOCK);
	struct sockaddr_in sa = { sizeof sa, AF_INET, htons(443) };
	inet_pton(AF_INET, ip, &sa.sin_addr);
	int kq = kqueue();
	{	// Network.framework watches reads (and EVFILT_SOCK) from before the connect
		struct kevent rv[2]; struct timespec z = {0, 0};
		EV_SET(&rv[0], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, 0);
		r = kevent(kq, rv, 1, NULL, 0, NULL);
		int n0 = kevent(kq, NULL, 0, rv, 2, &z);
		printf("; EVFILT_READ before connect: add %d, %d events", r, n0);
		for (int i = 0; i < n0; i++) printf(" [filter %d flags %#x fflags %#x data %ld]", rv[i].filter, rv[i].flags, rv[i].fflags, (long)rv[i].data);
	}
	r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
	printf("; connect %d errno %d\n", r, r ? errno : 0);
	struct kevent ev[2];
	EV_SET(&ev[0], fd, EVFILT_WRITE, EV_ADD, 0, 0, 0);
	EV_SET(&ev[1], fd, EVFILT_SOCK, EV_ADD, 0xffff, 0, 0);
	for (int i = 0; i < 2; i++) { r = kevent(kq, &ev[i], 1, NULL, 0, NULL); printf("raw: kevent add filter %d: %d errno %d\n", ev[i].filter, r, r ? errno : 0); }
	struct kevent out[4]; struct timespec ts = {1, 0};
	int n = 0;
	for (int round = 0; round < 4; round++) {
		n = kevent(kq, NULL, 0, out, 4, &ts);
		for (int i = 0; i < n; i++) printf("raw: round %d event filter %d flags %#x fflags %#x data %ld\n", round, out[i].filter, out[i].flags, out[i].fflags, (long)out[i].data);
		if (n > 0 && out[0].filter == EVFILT_WRITE) { EV_SET(&ev[0], fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0); kevent(kq, &ev[0], 1, NULL, 0, NULL); }
	}
	int err = 0; socklen_t l = sizeof err; getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
	printf("raw: %d events, SO_ERROR %d\n", n, err);
	close(kq); close(fd);
}

static void addrcb(DNSServiceRef r, DNSServiceFlags f, uint32_t ifi, DNSServiceErrorType e, const char *h, const struct sockaddr *sa, uint32_t ttl, void *ctx) {
	char b[64] = "-";
	if (sa && sa->sa_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)sa)->sin_addr, b, sizeof b);
	if (sa && sa->sa_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)sa)->sin6_addr, b, sizeof b);
	printf("dnssd: %s err=%d flags=%#x if=%u ttl=%u %s\n", h, e, f, ifi, ttl, b);
}

int main(int argc, char **argv) {
	@autoreleasepool {
		const char *host = argc > 1 ? argv[1] : "apple.com";
		NSString *url = argc > 2 ? @(argv[2]) : @"https://www.apple.com/";
		setvbuf(stdout, NULL, _IOLBF, 0);
		sleep(argc > 3 ? atoi(argv[3]) : 20);  // let the boot settle
		struct addrinfo hints = {0}, *res = NULL;
		hints.ai_socktype = SOCK_STREAM;
		int rc = getaddrinfo(host, "443", &hints, &res);
		printf("getaddrinfo(%s) = %d %s\n", host, rc, rc ? gai_strerror(rc) : "");
		for (struct addrinfo *a = res; a; a = a->ai_next) {
			char b[64]; void *p = a->ai_family == AF_INET ? (void *)&((struct sockaddr_in *)a->ai_addr)->sin_addr : (void *)&((struct sockaddr_in6 *)a->ai_addr)->sin6_addr;
			printf("  %s\n", inet_ntop(a->ai_family, p, b, sizeof b));
		}
		char firstv4[64] = "";
		for (struct addrinfo *a = res; a; a = a->ai_next) if (a->ai_family == AF_INET && !firstv4[0]) inet_ntop(AF_INET, &((struct sockaddr_in *)a->ai_addr)->sin_addr, firstv4, sizeof firstv4);
		if (res) freeaddrinfo(res);
		if (firstv4[0]) { rawconnect(firstv4, 0); rawconnect(firstv4, if_nametoindex("enp15s0") ?: 2); }

		DNSServiceRef ref = NULL;
		DNSServiceErrorType e = DNSServiceGetAddrInfo(&ref, kDNSServiceFlagsReturnIntermediates, 0, 0, host, addrcb, NULL);
		printf("DNSServiceGetAddrInfo = %d\n", e);
		if (!e) { for (int i = 0; i < 3; i++) { fd_set s; FD_ZERO(&s); int fd = DNSServiceRefSockFD(ref); FD_SET(fd, &s); struct timeval tv = {3, 0};
			if (select(fd + 1, &s, NULL, NULL, &tv) <= 0) break; DNSServiceProcessResult(ref); } DNSServiceRefDeallocate(ref); }

		int rs = socket(PF_ROUTE, SOCK_RAW, PF_ROUTE);
		printf("socket(PF_ROUTE) = %d %s\n", rs, rs < 0 ? strerror(errno) : "");
		if (rs >= 0) {
			struct { struct rt_msghdr h; struct sockaddr_in dst; } m = {0};
			m.h.rtm_msglen = sizeof m; m.h.rtm_version = RTM_VERSION; m.h.rtm_type = RTM_GET; m.h.rtm_addrs = RTA_DST; m.h.rtm_seq = 1; m.h.rtm_pid = getpid();
			m.dst.sin_len = sizeof m.dst; m.dst.sin_family = AF_INET; inet_pton(AF_INET, "17.253.144.10", &m.dst.sin_addr);
			ssize_t w = write(rs, &m, sizeof m); char buf[2048]; ssize_t n = read(rs, buf, sizeof buf);
			char ifn[IF_NAMESIZE] = "?"; if (n > 0) if_indextoname(((struct rt_msghdr *)buf)->rtm_index, ifn);
			printf("RTM_GET: wrote %zd read %zd index %d (%s) errno %d\n", w, n, n > 0 ? ((struct rt_msghdr *)buf)->rtm_index : -1, ifn, n > 0 ? ((struct rt_msghdr *)buf)->rtm_errno : errno);
			close(rs);
		}

		SCNetworkReachabilityRef reach = SCNetworkReachabilityCreateWithName(NULL, host);
		SCNetworkReachabilityFlags rf = 0;
		Boolean ok = SCNetworkReachabilityGetFlags(reach, &rf);
		printf("SCNetworkReachability(%s): ok=%d flags=%#x (reachable=%d)\n", host, ok, rf, !!(rf & kSCNetworkReachabilityFlagsReachable));

		dispatch_semaphore_t ps = dispatch_semaphore_create(0);
		nw_path_monitor_t mon = nw_path_monitor_create();
		nw_path_monitor_set_queue(mon, dispatch_get_global_queue(0, 0));
		__block int once = 0;
		nw_path_monitor_set_update_handler(mon, ^(nw_path_t path) {
			printf("nw_path: status=%d (%s)\n", nw_path_get_status(path), nw_path_get_status(path) == nw_path_status_satisfied ? "satisfied" : "not satisfied");
			NSLog(@"nw_path: %@", path);
			if (!once++) dispatch_semaphore_signal(ps);
		});
		nw_path_monitor_start(mon);
		dispatch_semaphore_wait(ps, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));

		dispatch_semaphore_t s = dispatch_semaphore_create(0);
		NSDate *t0 = [NSDate date];
		NSURLSessionDataTask *task = [[NSURLSession sharedSession] dataTaskWithURL:[NSURL URLWithString:url]
			completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
				printf("NSURLSession %s: status=%ld bytes=%lu in %.1fs error=%s\n", url.UTF8String, (long)[(NSHTTPURLResponse *)resp statusCode],
					(unsigned long)data.length, -[t0 timeIntervalSinceNow], err ? err.description.UTF8String : "none");
				dispatch_semaphore_signal(s);
			}];
		[task resume];
		if (dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC))) printf("NSURLSession: timed out after 60 s\n");
		printf("netprobe done\n");
	}
	return 0;
}
