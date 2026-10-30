// void _platform_bzero(void *dst, size_t n): n >= 256 -> native
	cmp x1, #256
	b.hs 9f
	mov x3, x0
1:	cmp x1, #8
	b.lo 2f
	str xzr, [x3], #8
	sub x1, x1, #8
	b 1b
2:	cbz x1, 8f
	strb wzr, [x3], #1
	sub x1, x1, #1
	b 2b
8:	ret
9:	svc #0
	ret
