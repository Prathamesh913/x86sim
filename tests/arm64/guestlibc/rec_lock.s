// os_unfair_recursive_lock_lock_with_options(lock, options) (Perf 18), as Darling's libplatform lock.c: an unowned lock
// is taken with the owner value (TSD slot 3); one this thread owns (OS_ULOCK_OWNER(cur) = cur | 1) bumps ourl_count
// (+4); contention goes native (svc #0)
	mrs x8, tpidrro_el0
	ldr w9, [x8, #24]
1:	ldaxr w10, [x0]
	cbnz w10, 2f
	stxr w11, w9, [x0]
	cbnz w11, 1b
	ret
2:	clrex
	orr w12, w10, #1
	cmp w12, w9
	b.ne 9f
	ldr w13, [x0, #4]
	add w13, w13, #1
	str w13, [x0, #4]
	ret
9:	svc #0
	ret
