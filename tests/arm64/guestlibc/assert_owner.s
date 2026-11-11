// os_unfair_lock_assert_owner(lock) (Perf 22): owned by this thread (value | 1 == TSD slot 3, as libplatform's
// OS_ULOCK_IS_NOT_OWNER) returns; otherwise the native function, which reports the failed assertion (svc #0)
	mrs x8, tpidrro_el0
	ldr w9, [x8, #24]
	ldr w10, [x0]
	orr w10, w10, #1
	cmp w10, w9
	b.ne 9f
	ret
9:	svc #0
	ret
