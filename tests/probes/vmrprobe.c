// x86Code: mach_vm_region (all flavors), mach_vm_region_recurse (short/full), vm_region_64, proc_listallpids,
// proc_listpids(PPID) - these used to abort darlingserver / return ENOTSUP.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <mach/mach.h>
kern_return_t mach_vm_region(vm_map_t, mach_vm_address_t*, mach_vm_size_t*, vm_region_flavor_t, vm_region_info_t, mach_msg_type_number_t*, mach_port_t*);
kern_return_t mach_vm_region_recurse(vm_map_t, mach_vm_address_t*, mach_vm_size_t*, natural_t*, vm_region_recurse_info_t, mach_msg_type_number_t*);
int proc_listallpids(void*, int); int proc_listpids(uint32_t, uint32_t, void*, int);
#define PROC_ALL_PIDS 1
#define PROC_PPID_ONLY 6
int main(void) {
	mach_vm_address_t a = 0; mach_vm_size_t sz; mach_port_t obj; kern_return_t kr; int n = 0;
	for (;;) {
		vm_region_basic_info_data_64_t b; mach_msg_type_number_t c = VM_REGION_BASIC_INFO_COUNT_64;
		kr = mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&b, &c, &obj);
		if (kr) break;
		if (n++ < 5) printf("basic %llx-%llx prot %x shared %d\n", a, a + sz, b.protection, b.shared);
		a += sz;
	}
	printf("basic: %d regions, end kr %d\n", n, kr);
	a = (mach_vm_address_t)&main;
	vm_region_extended_info_data_t e; mach_msg_type_number_t c = VM_REGION_EXTENDED_INFO_COUNT;
	kr = mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_EXTENDED_INFO, (vm_region_info_t)&e, &c, &obj);
	printf("extended kr %d %llx size %llx prot %x share %d res %u\n", kr, a, sz, e.protection, e.share_mode, e.pages_resident);
	vm_region_top_info_data_t t; c = VM_REGION_TOP_INFO_COUNT;
	a = (mach_vm_address_t)&main;
	kr = mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_TOP_INFO, (vm_region_info_t)&t, &c, &obj);
	printf("top kr %d share %d\n", kr, t.share_mode);
	a = 0; n = 0;
	for (;;) {
		vm_region_submap_info_data_64_t s; c = VM_REGION_SUBMAP_INFO_COUNT_64; natural_t d = 0;
		kr = mach_vm_region_recurse(mach_task_self(), &a, &sz, &d, (vm_region_recurse_info_t)&s, &c);
		if (kr) break;
		n++; a += sz;
	}
	printf("recurse full: %d regions, end kr %d\n", n, kr);
	vm_region_submap_short_info_data_64_t ss; c = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64; natural_t d = 0; a = 0;
	kr = mach_vm_region_recurse(mach_task_self(), &a, &sz, &d, (vm_region_recurse_info_t)&ss, &c);
	printf("recurse short kr %d %llx\n", kr, a);
	vm_address_t va = 0; vm_size_t vs; vm_region_basic_info_data_64_t b; c = VM_REGION_BASIC_INFO_COUNT_64;
	kr = vm_region_64(mach_task_self(), &va, &vs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&b, &c, &obj);
	printf("vm_region_64 kr %d %lx\n", kr, va);
	int pids[4096];
	int np = proc_listallpids(pids, sizeof(pids));
	printf("proc_listallpids %d (self %d in list:", np, getpid());
	int found = 0; for (int i = 0; i < np; i++) if (pids[i] == getpid()) found = 1;
	printf(" %d)\n", found);
	np = proc_listpids(PROC_PPID_ONLY, getppid(), pids, sizeof(pids));
	printf("proc_listpids(PPID %d) %d bytes\n", getppid(), np);
	printf("estimate %d\n", proc_listpids(PROC_ALL_PIDS, 0, NULL, 0));
	return 0;
}
