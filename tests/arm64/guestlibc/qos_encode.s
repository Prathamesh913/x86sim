// pthread_priority_t _pthread_qos_class_encode(qos_class_t qc, int relpri, unsigned long flags) (Perf 18): Darling's
// libpthread qos.c (_pthread_qos_class_to_thread_qos + _pthread_priority_make_from_thread_qos)
	and x8, x2, #0xff000000
	mov w9, #6
	cmp w0, #0x21
	b.eq 1f
	mov w9, #5
	cmp w0, #0x19
	b.eq 1f
	mov w9, #4
	cmp w0, #0x15
	b.eq 1f
	mov w9, #3
	cmp w0, #0x11
	b.eq 1f
	mov w9, #2
	cmp w0, #0x09
	b.eq 1f
	mov w9, #1
	cmp w0, #0x05
	b.eq 1f
	b 2f
1:	add w10, w9, #7
	mov w11, #1
	lsl w11, w11, w10
	orr x8, x8, x11
	sub w12, w1, #1
	and w12, w12, #0xff
	orr x8, x8, x12
2:	mov x0, x8
	ret
