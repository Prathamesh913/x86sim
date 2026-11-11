// kdebug_trace(...) / kdebug_is_enabled(debugid) (Perf 22): tracing is never enabled here (the commpage's KDEBUG_ENABLE
// is 0, as in Darling's libsyscall kdebug_trace.c), so both return 0 without the native round trip
	mov w0, #0
	ret
