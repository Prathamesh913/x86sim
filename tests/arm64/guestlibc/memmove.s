// void *_platform_memmove(void *dst, const void *src, size_t n): n >= 256 -> native (svc at the end, patched)
	cmp x2, #256
	b.hs 9f
	mov x3, x0
	cbz x2, 8f
	cmp x3, x1
	b.ls 5f
	add x4, x1, x2
	cmp x3, x4
	b.hs 5f
	add x1, x1, x2
	add x3, x3, x2
1:	cmp x2, #8
	b.lo 2f
	ldr x5, [x1, #-8]!
	str x5, [x3, #-8]!
	sub x2, x2, #8
	b 1b
2:	cbz x2, 8f
	ldrb w5, [x1, #-1]!
	strb w5, [x3, #-1]!
	sub x2, x2, #1
	b 2b
5:	cmp x2, #8
	b.lo 6f
	ldr x5, [x1], #8
	str x5, [x3], #8
	sub x2, x2, #8
	b 5b
6:	cbz x2, 8f
	ldrb w5, [x1], #1
	strb w5, [x3], #1
	sub x2, x2, #1
	b 6b
8:	ret
9:	svc #0
	ret
