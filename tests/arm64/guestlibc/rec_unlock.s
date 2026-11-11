// os_unfair_recursive_lock_unlock(lock) (Perf 18): a held count is decremented when this thread owns the lock; else an
// owned lock with no waiter bits is released; anything else goes native (svc #0: waiters, or the unowned-abort)
	mrs x8, tpidrro_el0
	ldr w9, [x8, #24]
	ldr w13, [x0, #4]
	cbz w13, 3f
	ldr w10, [x0]
	orr w12, w10, #1
	cmp w12, w9
	b.ne 9f
	sub w13, w13, #1
	str w13, [x0, #4]
	ret
3:	ldxr w10, [x0]
	cmp w10, w9
	b.ne 8f
	stlxr w11, wzr, [x0]
	cbnz w11, 3b
	ret
8:	clrex
9:	svc #0
	ret
