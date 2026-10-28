// x86Code: raw XPC (anonymous listener, same process): xpc_data of sizes around 16 KB, plain and
// from dispatch_data; the server echoes length + last byte (nsdprobe: NSData 16384..20000 arrived
// as the previous message's data on Darling).
#include <xpc/xpc.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern xpc_endpoint_t xpc_endpoint_create(xpc_connection_t);
extern xpc_object_t xpc_data_create_with_dispatch_data(dispatch_data_t);
int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	xpc_connection_t lis = xpc_connection_create(NULL, NULL);
	xpc_connection_set_event_handler(lis, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
		xpc_connection_set_event_handler(peer, ^(xpc_object_t m) {
			if (xpc_get_type(m) != XPC_TYPE_DICTIONARY) return;
			xpc_object_t r = xpc_dictionary_create_reply(m);
			size_t len = 0; const unsigned char* p = xpc_dictionary_get_data(m, "d", &len);
			xpc_dictionary_set_int64(r, "len", p ? (int64_t) len : -1);
			xpc_dictionary_set_int64(r, "last", p && len ? p[len - 1] : -1);
			xpc_connection_send_message(peer, r); xpc_release(r);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(lis);
	xpc_connection_t c = xpc_connection_create_from_endpoint(xpc_endpoint_create(lis));
	xpc_connection_set_event_handler(c, ^(xpc_object_t e) { (void) e; });
	xpc_connection_resume(c);
	size_t sizes[] = { 16000, 16383, 16384, 16385, 17000, 20000, 24000, 28000, 32767, 32768, 35274, 65536 };
	for (int mode = 0; mode < 2; mode++)
	for (int k = 0; k < sizeof(sizes) / sizeof(*sizes); k++) {
		size_t n = sizes[k];
		unsigned char* b = malloc(n); memset(b, 'a' + k, n);
		int bad = 0; long glen = 0, glast = 0;
		for (int i = 0; i < 20; i++) {
			xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
			if (mode == 0) xpc_dictionary_set_data(m, "d", b, n);
			else {
				dispatch_data_t dd = dispatch_data_create(b, n, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
				xpc_object_t xd = xpc_data_create_with_dispatch_data(dd);
				xpc_dictionary_set_value(m, "d", xd); xpc_release(xd); dispatch_release(dd);
			}
			xpc_object_t r = xpc_connection_send_message_with_reply_sync(c, m);
			glen = xpc_dictionary_get_int64(r, "len"); glast = xpc_dictionary_get_int64(r, "last");
			if (glen != (long) n || glast != 'a' + k) bad++;
			xpc_release(r); xpc_release(m);
		}
		printf("%s size %zu: %d/20 bad (last got len %ld byte %c)\n", mode ? "dispatch" : "plain", n, bad, glen, (int) glast);
		free(b);
	}
	printf("done\n");
	return 0;
}
