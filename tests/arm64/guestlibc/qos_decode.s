// qos_class_t _pthread_qos_class_decode(pthread_priority_t pp, int *relpri, unsigned long *flags) (Perf 18): Darling's
// libpthread qos.c (_pthread_priority_split: _pthread_priority_has_qos, ffs of the QoS bits, int8 relpri + 1)
	mov w9, #0
	mov w10, #0
	mov w14, #0x22000000
	tst x0, x14
	b.ne 3f
	tst x0, #0x3fc000
	b.ne 3f
	tst x0, #0x3f00
	b.eq 3f
	ubfx w11, w0, #8, #14
	rbit w11, w11
	clz w11, w11
	add w9, w11, #1
	sxtb w10, w0
	add w10, w10, #1
3:	cbz x2, 4f
	and x12, x0, #0xff000000
	str x12, [x2]
4:	cbz x1, 5f
	str w10, [x1]
5:	cmp w9, #7
	b.hs 6f
	adr x13, 7f
	ldrb w0, [x13, x9]
	ret
6:	mov w0, #0
	ret
7:	.byte 0x00, 0x05, 0x09, 0x11, 0x15, 0x19, 0x21, 0x00
