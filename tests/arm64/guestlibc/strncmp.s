// int _platform_strncmp(const char *a, const char *b, size_t n): byte loop (callers pass short strings)
	cbz x2, 9f
1:	ldrb w3, [x0], #1
	ldrb w4, [x1], #1
	subs w5, w3, w4
	b.ne 8f
	cbz w3, 9f
	subs x2, x2, #1
	b.ne 1b
9:	mov w0, #0
	ret
8:	mov w0, w5
	ret
