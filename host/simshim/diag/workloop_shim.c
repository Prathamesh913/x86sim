/*
 * Spike shim: Darling has no kernel workloops (kevent_id / EVFILT_WORKLOOP),
 * which iOS 26 libdispatch needs to service dispatch workloops. Interpose the
 * workloop constructors to return serial queues instead. Workloop-specific
 * semantics (per-item QoS, ownership, workgroups) are lost.
 *
 * Built with the iOS simulator SDK and injected with DYLD_INSERT_LIBRARIES.
 */
#include <dispatch/dispatch.h>
#include <os/workgroup.h>

#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } \
	_interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = \
	{ (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };


static dispatch_workloop_t shim_workloop_create(const char* label)
{
	return (dispatch_workloop_t) dispatch_queue_create(label, DISPATCH_QUEUE_SERIAL);
}

static dispatch_workloop_t shim_workloop_create_inactive(const char* label)
{
	return (dispatch_workloop_t) dispatch_queue_create(label,
		dispatch_queue_attr_make_initially_inactive(DISPATCH_QUEUE_SERIAL));
}

static void shim_workloop_set_autorelease_frequency(dispatch_workloop_t workloop, dispatch_autorelease_frequency_t frequency)
{
}

static void shim_workloop_set_os_workgroup(dispatch_workloop_t workloop, os_workgroup_t workgroup)
{
}

DYLD_INTERPOSE(shim_workloop_create, dispatch_workloop_create)
DYLD_INTERPOSE(shim_workloop_create_inactive, dispatch_workloop_create_inactive)
DYLD_INTERPOSE(shim_workloop_set_autorelease_frequency, dispatch_workloop_set_autorelease_frequency)
DYLD_INTERPOSE(shim_workloop_set_os_workgroup, dispatch_workloop_set_os_workgroup)
