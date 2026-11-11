// int pthread_main_np(void) (Perf 22): guest TSD slot 766 holds 1 on the main thread and 2 on others (set when the
// guest thread is created, from Darling's pthread_main_thread_np); 0 (unknown) goes native (svc #0)
	mrs x8, tpidrro_el0
	ldr x9, [x8, #6128]
	cbz x9, 9f
	cmp x9, #1
	cset w0, eq
	ret
9:	svc #0
	ret
