// x86Code: xpc_shmem_create_readonly snapshot semantics: does the shmem see writes made to the buffer
// (or to memory reusing its address) after creation? Compare Darling with macOS.
#include <xpc/xpc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <mach/mach.h>
xpc_object_t xpc_shmem_create_readonly(const void*, size_t);
static void test(const char* what, size_t sz, int mode) {
	unsigned char* b;
	if (mode == 0) b = mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0); else { vm_address_t a = 0; vm_allocate(mach_task_self(), &a, sz, VM_FLAGS_ANYWHERE); b = (void*)a; }
	memset(b, 'A', sz);
	xpc_object_t s = xpc_shmem_create_readonly(b, sz);
	printf("  created %p\n", s);
	if (getenv("WRITE")) memset(b, 'B', sz / 2);                    // write into the original buffer
	if (mode == 0) munmap(b, sz); else vm_deallocate(mach_task_self(), (vm_address_t)b, sz);
	unsigned char* c;
	if (mode == 0) c = mmap(b, sz, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0); else { vm_address_t a = 0; vm_allocate(mach_task_self(), &a, sz, VM_FLAGS_ANYWHERE); c = (void*)a; }
	memset(c, 'C', sz);                        // reuse
	void* m = NULL; size_t n = xpc_shmem_map(s, &m);
	unsigned char* p = m;
	printf("%s sz=%zu reused=%d: shmem n=%zu first=%c mid=%c last=%c\n", what, sz, c == b, n, p ? p[0] : '?', p ? p[sz / 2] : '?', p ? p[sz - 1] : '?');
}
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	size_t sizes[] = { 16384, 65536, 1 << 20, 4 << 20 };
	for (int i = 0; i < 4; i++) { test("mmap", sizes[i], 0); test("vm_allocate", sizes[i], 1); }
	return 0;
}
