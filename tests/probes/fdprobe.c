// x86Code: fileports: xpc_fd_create on a file, sent over an anonymous XPC connection, xpc_fd_dup'ed and read back
#include <xpc/xpc.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
extern xpc_endpoint_t xpc_endpoint_create(xpc_connection_t);
extern int fileport_makeport(int, mach_port_t*);
extern int fileport_makefd(mach_port_t);
int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	const char* path = "/tmp/fdprobe.txt";
	int f = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644); write(f, "hello fileport", 14); close(f);
	f = open(path, O_RDONLY);
	mach_port_t p = 0; int r = fileport_makeport(f, &p);
	int f2 = r == 0 ? fileport_makefd(p) : -1;
	char b[32] = {0}; if (f2 >= 0) pread(f2, b, 14, 0);
	printf("direct: makeport %d port 0x%x makefd %d read '%s'\n", r, p, f2, b);
	xpc_connection_t lis = xpc_connection_create(NULL, NULL);
	xpc_connection_set_event_handler(lis, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
		xpc_connection_set_event_handler(peer, ^(xpc_object_t m) {
			if (xpc_get_type(m) != XPC_TYPE_DICTIONARY) return;
			int fd = xpc_dictionary_dup_fd(m, "fd");
			char buf[32] = {0}; if (fd >= 0) pread(fd, buf, 14, 0);
			xpc_object_t rep = xpc_dictionary_create_reply(m);
			xpc_dictionary_set_string(rep, "got", fd >= 0 ? buf : "no fd");
			xpc_connection_send_message(peer, rep); xpc_release(rep);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(lis);
	xpc_connection_t c = xpc_connection_create_from_endpoint(xpc_endpoint_create(lis));
	xpc_connection_set_event_handler(c, ^(xpc_object_t e) { (void) e; });
	xpc_connection_resume(c);
	for (int i = 0; i < 3; i++) {
		xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
		xpc_dictionary_set_fd(m, "fd", f);
		xpc_object_t rep = xpc_connection_send_message_with_reply_sync(c, m);
		printf("xpc %d: %s\n", i, xpc_get_type(rep) == XPC_TYPE_DICTIONARY ? xpc_dictionary_get_string(rep, "got") : "error");
	}
	printf("done\n");
	return 0;
}
