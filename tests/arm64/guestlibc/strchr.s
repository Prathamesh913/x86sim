// char *_platform_strchr(const char *s, int c): byte loop (c == 0 finds the terminator)
	and w1, w1, #0xff
1:	ldrb w3, [x0]
	cmp w3, w1
	b.eq 8f
	cbz w3, 9f
	add x0, x0, #1
	b 1b
9:	mov x0, #0
8:	ret
