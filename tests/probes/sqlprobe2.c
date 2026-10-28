// x86Code: two processes on one WAL database. Run twice (two jobs): the first to create
// <TMPDIR>/sqlprobe2.claim holds the db open with a write; the other reads and writes.
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

static void step(const char* who, sqlite3* db, const char* sql)
{
	char* err = NULL;
	int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
	printf("%s: %-45s -> %d %s (errno %d)\n", who, sql, rc, err ? err : "ok", sqlite3_system_errno(db));
	sqlite3_free(err);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	const char* tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	char path[1024], claim[1024];
	snprintf(path, sizeof(path), "%s/sqlprobe2.db", tmp);
	snprintf(claim, sizeof(claim), "%s/sqlprobe2.claim", tmp);
	int first = open(claim, O_CREAT | O_EXCL | O_WRONLY, 0644) >= 0;
	const char* who = first ? "first" : "second";
	sqlite3* db;
	if (first)
	{
		unlink(path);
		printf("first: open -> %d\n", sqlite3_open(path, &db));
		step(who, db, "PRAGMA journal_mode=WAL");
		step(who, db, "CREATE TABLE t(a)");
		step(who, db, "INSERT INTO t VALUES(1)");
		sleep(20);
		step(who, db, "INSERT INTO t VALUES(3)");
		step(who, db, "SELECT count(*) FROM t");
	}
	else
	{
		sleep(5);
		printf("second: open -> %d\n", sqlite3_open(path, &db));
		sqlite3_busy_timeout(db, 3000);
		step(who, db, "SELECT count(*) FROM t");
		step(who, db, "INSERT INTO t VALUES(2)");
		step(who, db, "CREATE TABLE IF NOT EXISTS u(x)");
	}
	sqlite3_close(db);
	return 0;
}
