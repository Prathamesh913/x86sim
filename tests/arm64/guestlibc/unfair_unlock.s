// void _os_unfair_lock_unlock(os_unfair_lock *l): owned by this thread with no waiter bits -> store 0; anything else
// (waiters to wake, not the owner) goes to the native function (svc #0), which also does the ownership check.
	mrs x8, tpidrro_el0
	ldr w9, [x8, #24]
1:	ldxr w10, [x0]
	cmp w10, w9
	b.ne 9f
	stlxr w11, wzr, [x0]
	cbnz w11, 1b
	ret
9:	clrex
	svc #0
	ret
