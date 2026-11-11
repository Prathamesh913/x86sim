// malloc_default_zone() (Perf 17): the guest-side default zone, from this thread's gheap Cache (+512: &default zone)
	mrs x9, tpidrro_el0
	ldr x9, [x9, #6136]
	cbz x9, 9f
	ldr x10, [x9, #512]
	ldr x0, [x10]
	ret
9:	svc #0
	ret
