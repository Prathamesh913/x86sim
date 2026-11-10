// size_t _platform_strnlen(const char *s, size_t max): byte loop
	mov x2, x0
	cbz x1, 9f
1:	ldrb w3, [x2]
	cbz w3, 9f
	add x2, x2, #1
	subs x1, x1, #1
	b.ne 1b
9:	sub x0, x2, x0
	ret
