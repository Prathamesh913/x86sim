// calloc(n, size) / malloc_type_calloc(n, size, type): as malloc (Perf 16) for n * size <= 32 KB (blocks are zeroed)
	mrs x9, tpidrro_el0
	ldr x9, [x9, #6136]
	cbz x9, 9f
	umulh x8, x0, x1
	cbnz x8, 9f
	mul x8, x0, x1
	cmp x8, #8, lsl #12
	b.hi 9f
	add x10, x8, #15
	lsr x10, x10, #4
	ldr x11, [x9, #480]
	ldrb w10, [x11, x10]
	add x13, x9, x10, lsl #3
	ldr x12, [x13]
	cbz x12, 9f
	ldr x14, [x12]
	str x14, [x13]
	add x15, x9, #320
	ldr w16, [x15, x10, lsl #2]
	sub w16, w16, #1
	str w16, [x15, x10, lsl #2]
	stp xzr, xzr, [x12]
	mov x0, x12
	ret
9:	svc #0
	ret
