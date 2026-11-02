// x86Code: diagnostic shim (Speed 30, arm64): one AttributeGraph attribute's life. AGNODE_ID=<decimal attribute id>
// (the id in "attribute failed to set an initial value: <id>"). Logs the guest return addresses (raw, unslid cache
// addresses: symbolicate with `ipsw dyld a2s`) of AGGraphCreateAttribute calls that return it, of AGGraphSetValue /
// AGGraphCreateOffsetAttribute2 on it, and a count of creations. Log: /private/var/tmp/x86code-a64/agnode.<prog>.<pid>.log (next to the JIT logs)
// Loaded with DYLD_INSERT_LIBRARIES (e.g. per process through libarm64jit's a64-penv file).
#include "diagpath.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <execinfo.h>
#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };

extern uint32_t AGGraphCreateAttribute(uint32_t type, const void* body, const void* value);
extern bool AGGraphSetValue(uint32_t attr, const void* value, const void* type);
extern uint32_t AGGraphCreateIndirectAttribute(uint32_t attr);
extern uint32_t AGGraphCreateIndirectAttribute2(uint32_t attr, uint64_t size);

static uint32_t want;
static int lfd = -1;
static uint64_t ncreate;
static void lg(const char* fmt, ...)
{
	if (lfd < 0) return;
	char b[600];
	int n = snprintf(b, sizeof(b), "[%x] ", (unsigned) pthread_mach_thread_np(pthread_self()));
	va_list ap; va_start(ap, fmt); n += vsnprintf(b + n, sizeof(b) - n, fmt, ap); va_end(ap);
	if (n > (int) sizeof(b) - 2) n = sizeof(b) - 2;
	b[n++] = '\n'; write(lfd, b, n);
}
static void bt(const char* what, uint32_t a)
{
	void* f[24]; int k = backtrace(f, 24);
	char b[24 * 14 + 64]; int n = 0;
	for (int i = 1; i < k; i++) n += snprintf(b + n, sizeof(b) - n, " %lx", (unsigned long) f[i]);
	lg("%s %u:%s", what, a, b);
}
static uint32_t my_create(uint32_t type, const void* body, const void* value)
{
	uint32_t r = AGGraphCreateAttribute(type, body, value);
	if (want) {
		ncreate++;
		if (r == want) { lg("create type %u value %p (creation #%llu)", type, value, (unsigned long long) ncreate); bt("create", r); }
	}
	return r;
}
DYLD_INTERPOSE(my_create, AGGraphCreateAttribute)
static bool my_setvalue(uint32_t attr, const void* value, const void* type)
{
	bool r = AGGraphSetValue(attr, value, type);
	if (want && attr == want) { lg("setValue -> %d", r); bt("setValue", attr); }
	return r;
}
DYLD_INTERPOSE(my_setvalue, AGGraphSetValue)
static uint32_t my_ind(uint32_t attr)
{
	uint32_t r = AGGraphCreateIndirectAttribute(attr);
	if (want && (attr == want || r == want)) { lg("indirect %u -> %u", attr, r); bt("indirect", attr); }
	return r;
}
DYLD_INTERPOSE(my_ind, AGGraphCreateIndirectAttribute)
static uint32_t my_ind2(uint32_t attr, uint64_t size)
{
	uint32_t r = AGGraphCreateIndirectAttribute2(attr, size);
	if (want && (attr == want || r == want)) { lg("indirect2 %u -> %u", attr, r); bt("indirect2", attr); }
	return r;
}
DYLD_INTERPOSE(my_ind2, AGGraphCreateIndirectAttribute2)
__attribute__((constructor)) static void agnode_init(void)
{
	const char* e = getenv("AGNODE_ID");
	if (!e) return;
	want = (uint32_t) strtoul(e, NULL, 0);
	char p[512];
	snprintf(p, sizeof(p), "/private/var/tmp/x86code-a64/agnode.%s.%d.log", getprogname(), getpid());
	lfd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
	lg("agnode loaded, watching %u", want);
}
