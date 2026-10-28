// x86Code: _PASSqliteDatabase-style WAL database (ATXInformationStore): open, WAL,
// BEGIN EXCLUSIVE / CREATE / COMMIT, with one and two connections, extended error codes;
// busy_timeout set (Apple SQLite then locks with F_OFD_SETLKWTIMEOUT), plus one timed-out conflict
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
static void step(const char* who, sqlite3* db, const char* sql)
{
	char* err = NULL;
	int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
	printf("%s: %-50s -> %d/%d %s (errno %d)\n", who, sql, rc, sqlite3_extended_errcode(db), err ? err : "ok", sqlite3_system_errno(db));
	sqlite3_free(err);
}
int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	const char* tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	char path[1024], p2[1100];
	snprintf(path, sizeof(path), "%s/sqlprobe3.db", tmp);
	for (int i = 0; i < 3; i++) { static const char* sfx[] = { "", "-wal", "-shm" }; snprintf(p2, sizeof(p2), "%s%s", path, sfx[i]); unlink(p2); }
	sqlite3 *a, *b;
	int fl = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
	printf("open a -> %d\n", sqlite3_open_v2(path, &a, fl, NULL));
	step("a", a, "PRAGMA busy_timeout=1500");
	step("a", a, "PRAGMA journal_mode=WAL");
	step("a", a, "PRAGMA user_version");
	step("a", a, "BEGIN EXCLUSIVE");
	step("a", a, "CREATE TABLE t(x)");
	step("a", a, "COMMIT");
	step("a", a, "BEGIN EXCLUSIVE");
	step("a", a, "INSERT INTO t VALUES(1)");
	step("a", a, "COMMIT");
	printf("open b -> %d\n", sqlite3_open_v2(path, &b, fl, NULL));
	step("b", b, "PRAGMA busy_timeout=1500");
	step("b", b, "SELECT count(*) FROM t");
	step("b", b, "BEGIN EXCLUSIVE");
	step("b", b, "INSERT INTO t VALUES(2)");
	step("b", b, "COMMIT");
	step("a", a, "BEGIN IMMEDIATE");
	time_t t0 = time(NULL);
	step("b", b, "BEGIN IMMEDIATE"); // conflicts with a: BUSY after ~1.5 s
	printf("b waited ~%ld s\n", (long)(time(NULL) - t0));
	step("a", a, "COMMIT");
	sqlite3_close(b);
	step("a", a, "PRAGMA wal_checkpoint(TRUNCATE)");
	sqlite3_close(a);
	return 0;
}
