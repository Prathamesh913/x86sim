// x86Code research: symbols Apple's Metal toolchain (macOS 14+) imports that Darling's libSystem/libc++ lack.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void _os_log_error_impl(void *dso, void *log, int type, const char *fmt, void *buf, unsigned size) {
	(void)dso; (void)log; (void)type; (void)buf; (void)size; fprintf(stderr, "[os_log] %s\n", fmt);
}
__attribute__((noreturn)) void _ZNSt3__122__libcpp_verbose_abortEPKcz(const char *fmt, ...) {
	va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); abort();
}
