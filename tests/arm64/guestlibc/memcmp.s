// int _platform_memcmp(const void *a, const void *b, size_t n)
1:	cmp x2, #8
	b.lo 3f
	ldr x4, [x0]
	ldr x5, [x1]
	cmp x4, x5
	b.ne 2f
	add x0, x0, #8
	add x1, x1, #8
	sub x2, x2, #8
	b 1b
2:	eor x6, x4, x5
	rbit x6, x6
	clz x6, x6
	and x6, x6, #0x38
	lsr x4, x4, x6
	lsr x5, x5, x6
	and w4, w4, #0xff
	and w5, w5, #0xff
	sub w0, w4, w5
	ret
3:	cbz x2, 4f
	ldrb w4, [x0], #1
	ldrb w5, [x1], #1
	sub x2, x2, #1
	cmp w4, w5
	b.eq 3b
	sub w0, w4, w5
	ret
4:	mov w0, #0
	ret
