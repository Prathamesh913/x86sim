// size_t strlen(const char *s): bytes up to 8-alignment, then 8 at a time with the has-zero-byte trick
	mov x1, x0
1:	tst x1, #7
	b.eq 2f
	ldrb w2, [x1]
	cbz w2, 5f
	add x1, x1, #1
	b 1b
2:	mov x4, #0x0101010101010101
3:	ldr x2, [x1]
	sub x3, x2, x4
	bic x3, x3, x2
	ands x3, x3, #0x8080808080808080
	b.ne 4f
	add x1, x1, #8
	b 3b
4:	rbit x3, x3
	clz x3, x3
	add x1, x1, x3, lsr #3
5:	sub x0, x1, x0
	ret
