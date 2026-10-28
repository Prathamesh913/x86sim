// Spike 3.2 client: write and read back a preference through cfprefsd (XPC)
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <xpc/xpc.h>

// Marked unavailable on iOS in the SDK, but present in libxpc (system processes use it)
extern xpc_connection_t x86code_create_mach_service(const char* name, dispatch_queue_t q, uint64_t flags)
	__asm__("_xpc_connection_create_mach_service");

extern kern_return_t bootstrap_look_up(mach_port_t bp, const char* service_name, mach_port_t* sp);

int main(void)
{
	mach_port_t bs = MACH_PORT_NULL, svc = MACH_PORT_NULL;
	task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &bs);
	kern_return_t kr = bootstrap_look_up(bs, "com.apple.cfprefsd.daemon", &svc);
	printf("bootstrap port 0x%x, look_up(com.apple.cfprefsd.daemon): 0x%x port 0x%x\n", bs, kr, svc);
	fflush(stdout);

	// Raw XPC round trip to cfprefsd (an unknown request still gets a reply or an error)
	xpc_connection_t conn = x86code_create_mach_service("com.apple.cfprefsd.daemon", NULL, 0);
	xpc_connection_set_event_handler(conn, ^(xpc_object_t ev) {
		char* d = xpc_copy_description(ev);
		printf("xpc event: %s\n", d);
		free(d);
	});
	xpc_connection_resume(conn);
	xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_int64(msg, "CFPreferencesOperation", 0);
	xpc_object_t reply = xpc_connection_send_message_with_reply_sync(conn, msg);
	char* d = xpc_copy_description(reply);
	printf("xpc reply: %s\n", d);
	free(d);
	fflush(stdout);

	CFStringRef app = CFSTR("com.x86code.prefstest");
	CFStringRef key = CFSTR("greeting");
	CFPreferencesSetAppValue(key, CFSTR("hello from linux"), app);
	Boolean synced = CFPreferencesAppSynchronize(app);
	printf("synchronize: %d\n", synced);

	CFPropertyListRef value = CFPreferencesCopyAppValue(key, app);
	char buf[128] = "(null)";
	if (value != NULL && CFGetTypeID(value) == CFStringGetTypeID())
		CFStringGetCString(value, buf, sizeof(buf), kCFStringEncodingUTF8);
	printf("read back: %s\n", buf);
	fflush(stdout);
	return value != NULL ? 0 : 1;
}
