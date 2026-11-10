// void _os_unfair_lock_lock(os_unfair_lock *l) (and _with_options: x1 kept for the slow path): an unowned lock is
// taken with one exclusive pair, storing the owner value Darwin's libplatform uses (TSD slot 3, the thread's mach
// port, which the guest TSD shares with the native one); anything else goes to the native function (svc #0).
	mrs x8, tpidrro_el0
	ldr w9, [x8, #24]
1:	ldaxr w10, [x0]
	cbnz w10, 9f
	stxr w11, w9, [x0]
	cbnz w11, 1b
	ret
9:	clrex
	svc #0
	ret
