// malloc(size) / malloc_type_malloc(size, type): small sizes from this thread's gheap cache (Perf 16). TSD slot 767:
// the thread's gheap Cache (gheap.cpp: head[40] @0, count[40] u32 @320, class_of @480, chunk_class @488,
// class_size @496, limit @504); 0 until the native handler first runs on this thread. Free blocks are zero except
// their 16-byte header, which this clears. Anything else: the native handler (svc #0).
	mrs x9, tpidrro_el0
	ldr x9, [x9, #6136]
	cbz x9, 9f
	cmp x0, #8, lsl #12
	b.hi 9f
	add x10, x0, #15
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
