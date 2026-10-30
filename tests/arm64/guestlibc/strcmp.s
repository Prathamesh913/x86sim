// int _platform_strcmp(const char *a, const char *b): 8 bytes at a time while neither load crosses a page
	mov x6, #0x0101010101010101
	mov x7, #0x8080808080808080
1:	and x4, x0, #0xfff
	cmp x4, #0xff8
	b.hi 3f
	and x4, x1, #0xfff
	cmp x4, #0xff8
	b.hi 3f
	ldr x2, [x0]
	ldr x3, [x1]
	sub x4, x2, x6
	bic x4, x4, x2
	and x4, x4, x7
	eor x5, x2, x3
	orr x5, x5, x4
	cbnz x5, 2f
	add x0, x0, #8
	add x1, x1, #8
	b 1b
2:	rbit x5, x5
	clz x5, x5
	and x5, x5, #0x38
	lsr x2, x2, x5
	lsr x3, x3, x5
	and w2, w2, #0xff
	and w3, w3, #0xff
	sub w0, w2, w3
	ret
3:	ldrb w2, [x0], #1
	ldrb w3, [x1], #1
	cmp w2, #1
	ccmp w2, w3, #0, hs
	b.eq 1b
	sub w0, w2, w3
	ret
