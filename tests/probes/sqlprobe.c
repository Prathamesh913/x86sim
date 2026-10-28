// x86Code: does SQLite work in the simulator under Darling? (create, write, read, WAL)
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static void step(sqlite3* db, const char* sql)
{
	char* err = NULL;
	int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
	printf("%-60s -> %d %s (errno %d)\n", sql, rc, err ? err : "ok", sqlite3_system_errno(db));
	sqlite3_free(err);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	char path[1024];
	snprintf(path, sizeof(path), "%s/sqlprobe.db", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	unlink(path);
	sqlite3* db;
	int rc = sqlite3_open(path, &db);
	printf("open %s -> %d (%s)\n", path, rc, sqlite3_errmsg(db));
	step(db, "CREATE TABLE t(a INTEGER, b TEXT)");
	step(db, "INSERT INTO t VALUES(1, 'x')");
	step(db, "SELECT count(*) FROM t");
	step(db, "PRAGMA journal_mode=WAL");
	step(db, "INSERT INTO t VALUES(2, 'y')");
	step(db, "BEGIN EXCLUSIVE; INSERT INTO t VALUES(3,'z'); COMMIT");
	// second process on the same WAL database while we keep ours open
	pid_t child = fork();
	if (child == 0)
	{
		sqlite3* db2;
		int rc2 = sqlite3_open(path, &db2);
		printf("child open -> %d\n", rc2);
		step(db2, "SELECT count(*) FROM t");
		step(db2, "INSERT INTO t VALUES(4, 'child')");
		step(db2, "CREATE TABLE IF NOT EXISTS u(x)");
		sqlite3_close(db2);
		_exit(0);
	}
	int status;
	waitpid(child, &status, 0);
	step(db, "INSERT INTO t VALUES(5, 'parent after child')");
	sqlite3_close(db);
	return 0;
}
