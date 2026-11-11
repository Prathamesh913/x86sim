// size_t malloc_size(const void *p) (Perf 22): a gheap small block's class size (chunk class table, as free.s);
// anything else (large, foreign, NULL) goes to the native handler (svc #0)
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
	ldr x16, [x9, #496]
	ldr w0, [x16, x13, lsl #2]
	ret
9:	svc #0
	ret
