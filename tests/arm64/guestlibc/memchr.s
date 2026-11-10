// void *_platform_memchr(const void *s, int c, size_t n): the first 64 bytes as a byte loop (callers often pass a large
// n and find c early); the rest of a longer buffer goes to the native function with s and n advanced (svc #0)
	and w1, w1, #0xff
	cbz x2, 9f
	mov x4, #64
1:	ldrb w3, [x0]
	cmp w3, w1
	b.eq 8f
	add x0, x0, #1
	subs x2, x2, #1
	b.eq 9f
	subs x4, x4, #1
	b.ne 1b
	svc #0
	ret
9:	mov x0, #0
8:	ret
