// malloc_zone_malloc(zone, size) / malloc_type_zone_malloc(zone, size, type) (Perf 17): malloc.s for the default zone
	mrs x9, tpidrro_el0
	ldr x9, [x9, #6136]
	cbz x9, 9f
	ldr x10, [x9, #512]
	ldr x10, [x10]
	cmp x0, x10
	b.ne 9f
	cmp x1, #8, lsl #12
	b.hi 9f
	add x10, x1, #15
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
