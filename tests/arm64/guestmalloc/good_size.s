// malloc_good_size(size) (Perf 17): a small size's class size, from the Cache's tables; larger sizes native
	mrs x9, tpidrro_el0
	ldr x9, [x9, #6136]
	cbz x9, 9f
	cmp x0, #8, lsl #12
	b.hi 9f
	add x10, x0, #15
	lsr x10, x10, #4
	ldr x11, [x9, #480]
	ldrb w10, [x11, x10]
	ldr x11, [x9, #496]
	ldr w0, [x11, x10, lsl #2]
	ret
9:	svc #0
	ret
