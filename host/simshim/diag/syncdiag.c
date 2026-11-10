// Diagnostic shim: logs every xpc_connection_send_message_with_reply_sync (service name,
// message, reply time) to stderr, before and after, so a hang shows the last request
#include <xpc/xpc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
extern pid_t xpc_connection_get_pid_x(xpc_connection_t) __asm__("_xpc_connection_get_pid");

#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };

static xpc_object_t d_sync(xpc_connection_t c, xpc_object_t m)
{
	char line[700];
	const char* name = xpc_connection_get_name(c);
	char* desc = xpc_copy_description(m);
	char* cdesc = xpc_copy_description(c);
	if (cdesc) { for (char* p = cdesc; *p; p++) if (*p == '\n' || *p == '\t') *p = ' ';
		int k = snprintf(line, sizeof(line), "syncdiag: conn %.600s on queue %s\n", cdesc, dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL)); write(2, line, k < (int) sizeof(line) ? k : (int) sizeof(line) - 1); free(cdesc); }
	uint64_t tid = 0;
	pthread_threadid_np(NULL, &tid);
	for (char* p = desc; p && *p; p++) if (*p == '\n' || *p == '\t') *p = ' ';
	int n = snprintf(line, sizeof(line), "syncdiag[%llu]: -> %s pid %d: %.500s\n", tid, name ? name : "(anon)", xpc_connection_get_pid_x(c), desc ? desc : "");
	write(2, line, n < (int) sizeof(line) ? n : (int) sizeof(line) - 1);
	free(desc);
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	xpc_object_t r = xpc_connection_send_message_with_reply_sync(c, m);
	clock_gettime(CLOCK_MONOTONIC, &b);
	n = snprintf(line, sizeof(line), "syncdiag[%llu]: <- %s %.3fs %s\n", tid, name ? name : "(anon)",
		(b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9, xpc_get_type(r) == XPC_TYPE_ERROR ? "ERROR" : "ok");
	write(2, line, n);
	return r;
}
DYLD_INTERPOSE(d_sync, xpc_connection_send_message_with_reply_sync)

// x86Sim: log who calls exit/_exit (SpringBoard exit(3) loop)
#include <execinfo.h>
static void exit_bt(const char* what, int code)
{
	char line[100];
	void* f[64];
	int n = snprintf(line, sizeof(line), "syncdiag: %s(%d) from:\n", what, code);
	write(2, line, n);
	backtrace_symbols_fd(f, backtrace(f, 64), 2);
}
static void d_exit(int code) { exit_bt("exit", code); exit(code); }
static void d__exit(int code) { exit_bt("_exit", code); _exit(code); }
DYLD_INTERPOSE(d_exit, exit)
DYLD_INTERPOSE(d__exit, _exit)

// x86Sim: log failing sqlite3_step / sqlite3_open_v2 (with SQL, extended code, errno)
#include <sqlite3.h>
static int d_sqlite3_step(sqlite3_stmt* s)
{
	int rc = sqlite3_step(s);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE && rc != SQLITE_OK) {
		sqlite3* db = sqlite3_db_handle(s);
		char line[1200];
		int n = snprintf(line, sizeof(line), "syncdiag: sqlite3_step %s: rc %d ext %d errno %d '%s' sql: %.700s\n", sqlite3_db_filename(db, "main"),
			rc, sqlite3_extended_errcode(db), sqlite3_system_errno(db), sqlite3_errmsg(db), sqlite3_sql(s));
		write(2, line, n < (int) sizeof(line) ? n : (int) sizeof(line) - 1);
	}
	return rc;
}
static int d_sqlite3_open_v2(const char* f, sqlite3** db, int fl, const char* vfs)
{
	int rc = sqlite3_open_v2(f, db, fl, vfs);
	char line[1200];
	int n = snprintf(line, sizeof(line), "syncdiag: sqlite3_open_v2 %s flags 0x%x vfs %s -> %d\n", f, fl, vfs ? vfs : "-", rc);
	write(2, line, n < (int) sizeof(line) ? n : (int) sizeof(line) - 1);
	return rc;
}
DYLD_INTERPOSE(d_sqlite3_step, sqlite3_step)
DYLD_INTERPOSE(d_sqlite3_open_v2, sqlite3_open_v2)

// x86Sim: SQLite's own error log (statement abort sites, I/O errors)
static void sqlog(void* u, int rc, const char* msg)
{
	char line[1000];
	int n = snprintf(line, sizeof(line), "syncdiag: sqlite log %d: %.900s\n", rc, msg);
	write(2, line, n < (int) sizeof(line) ? n : (int) sizeof(line) - 1);
}
__attribute__((constructor)) static void sqlog_init(void) { sqlite3_config(SQLITE_CONFIG_LOG, sqlog, NULL); }
