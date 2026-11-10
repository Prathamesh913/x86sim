// Diagnostic shim: logs entitlement lookups (key, result) made through libxpc and
// libsystem_containermanager to /private/tmp/x86sim-entdiag.<pid>.log
#include <xpc/xpc.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };

extern xpc_object_t xpc_copy_entitlement_for_token(const char* key, const void* token);
extern xpc_object_t xpc_copy_entitlement_for_self(const char* key);
extern xpc_object_t xpc_connection_copy_entitlement_value(xpc_connection_t c, const char* key);
extern xpc_object_t container_audit_token_copy_entitlement(const void* token, const char* key);

static void logline(const char* fn, const char* key, const void* token, xpc_object_t v)
{
	char path[64], line[512], *desc = v ? xpc_copy_description(v) : NULL;
	snprintf(path, sizeof(path), "/private/tmp/x86sim-entdiag.%d.log", getpid());
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	int pid = token ? ((const int*) token)[5] : -1;
	int n = snprintf(line, sizeof(line), "%s(%s, pid %d) -> %.200s\n", fn, key ? key : "(null)", pid, desc ? desc : "NULL");
	if (n >= (int) sizeof(line)) n = sizeof(line) - 1;
	if (fd >= 0) { write(fd, line, n); close(fd); }
	write(2, line, n);
	free(desc);
}

static xpc_object_t d_token(const char* key, const void* token)
{
	xpc_object_t v = xpc_copy_entitlement_for_token(key, token);
	logline("xpc_copy_entitlement_for_token", key, token, v);
	return v;
}
DYLD_INTERPOSE(d_token, xpc_copy_entitlement_for_token)

static xpc_object_t d_conn(xpc_connection_t c, const char* key)
{
	xpc_object_t v = xpc_connection_copy_entitlement_value(c, key);
	logline("xpc_connection_copy_entitlement_value", key, NULL, v);
	return v;
}
DYLD_INTERPOSE(d_conn, xpc_connection_copy_entitlement_value)

static xpc_object_t d_cm(const void* token, const char* key)
{
	xpc_object_t v = container_audit_token_copy_entitlement(token, key);
	logline("container_audit_token_copy_entitlement", key, token, v);
	return v;
}
DYLD_INTERPOSE(d_cm, container_audit_token_copy_entitlement)

__attribute__((constructor)) static void loaded(void)
{
	static const char msg[] = "entdiag: loaded\n";
	write(2, msg, sizeof(msg) - 1);
}

extern int csops_audittoken(int pid, unsigned int ops, void* useraddr, size_t usersize, const void* token);
static int d_csops_at(int pid, unsigned int ops, void* useraddr, size_t usersize, const void* token)
{
	int r = csops_audittoken(pid, ops, useraddr, usersize, token);
	int e = errno;
	char line[256];
	const unsigned* t = token;
	int n = snprintf(line, sizeof(line), "csops_audittoken(pid %d, op %u, size %zu, tok pid %u ver %u) -> %d errno %d hdr %08x %08x\n",
		pid, ops, usersize, t ? t[5] : 0, t ? t[7] : 0, r, r ? e : 0,
		usersize >= 8 && useraddr ? ((unsigned*)useraddr)[0] : 0, usersize >= 8 && useraddr ? ((unsigned*)useraddr)[1] : 0);
	write(2, line, n);
	errno = e;
	return r;
}
DYLD_INTERPOSE(d_csops_at, csops_audittoken)

extern xpc_object_t container_client_copy_entitlement(const char* key, const void* client);
static xpc_object_t d_cce(const char* key, const void* client)
{
	xpc_object_t v = container_client_copy_entitlement(key, client);
	logline("container_client_copy_entitlement", key, NULL, v);
	return v;
}
DYLD_INTERPOSE(d_cce, container_client_copy_entitlement)
