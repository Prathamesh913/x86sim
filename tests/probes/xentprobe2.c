// x86Code: xpc_copy_entitlement_for_token for each process (as containermanagerd does)
#include <xpc/xpc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
int proc_pidpath(int pid, void* buf, uint32_t size);
xpc_object_t xpc_copy_entitlement_for_token(const char* key, audit_token_t* token);

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	sleep(10);
	for (int pid = 20; pid < 200; pid++) {
		char path[1024] = "";
		if (proc_pidpath(pid, path, sizeof(path)) <= 0)
			continue;
		audit_token_t t;
		memset(&t, 0, sizeof(t));
		t.val[5] = pid;
		xpc_object_t all = xpc_copy_entitlement_for_token(NULL, &t);
		xpc_object_t one = xpc_copy_entitlement_for_token("com.apple.runningboard.primitiveattribute", &t);
		printf("%d %s: all=%s count=%zu primitiveattribute=%s\n", pid, strrchr(path, '/') + 1,
			all ? xpc_type_get_name(xpc_get_type(all)) : "NULL",
			all && xpc_get_type(all) == XPC_TYPE_DICTIONARY ? xpc_dictionary_get_count(all) : 0,
			one ? xpc_copy_description(one) : "NULL");
	}
	return 0;
}
