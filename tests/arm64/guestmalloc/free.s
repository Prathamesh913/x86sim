// free(p) / malloc_type_free(p, type) (Perf 16): a small block of at most 256 bytes owned by gheap goes on this
// thread's cache list (zeroed, free marker in word 1) while the class is under its limit; NULL returns; anything else
// (large blocks, foreign pointers, a second free, a full cache, bigger classes) goes to the native handler (svc #0).
	cbz x0, 8f
	mrs x9, tpidrro_el0
	ldr x9, [x9, #6136]
	cbz x9, 9f
	mov x10, #0x300000000
	subs x11, x0, x10
	b.lo 9f
	lsr x11, x11, #20
	mov x16, #0xcc00
	cmp x11, x16
	b.hs 9f
	ldr x12, [x9, #488]
	ldrb w13, [x12, x11]
	cbz w13, 9f
	sub w13, w13, #1
	movz x15, #0x4f63
	movk x15, #0x0a7b, lsl #16
	movk x15, #0x8e1d, lsl #32
	movk x15, #0x5f2c, lsl #48
	ldr x14, [x0, #8]
	cmp x14, x15
	b.eq 9f
	ldr x16, [x9, #504]
	ldr w16, [x16, x13, lsl #2]
	add x17, x9, #320
	ldr w8, [x17, x13, lsl #2]
	cmp w8, w16
	b.hs 9f
	ldr x16, [x9, #496]
	ldr w16, [x16, x13, lsl #2]
	cmp w16, #256
	b.hi 9f
	mov x14, x0
1:	stp xzr, xzr, [x14], #16
	subs w16, w16, #16
	b.ne 1b
	str x15, [x0, #8]
	ldr x14, [x9, x13, lsl #3]
	str x14, [x0]
	str x0, [x9, x13, lsl #3]
	add w8, w8, #1
	str w8, [x17, x13, lsl #2]
8:	ret
9:	svc #0
	ret
