// x86Code: replay SpringBoard's failing applicationState.db statements on a copy
// (<TMPDIR>/sqlprobe4.db[-wal]) with SpringBoard's open flags
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
static void step(sqlite3* db, const char* sql)
{
	char* err = NULL;
	int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
	printf("%-70.70s -> %d/%d %s (errno %d)\n", sql, rc, sqlite3_extended_errcode(db), err ? err : "ok", sqlite3_system_errno(db));
	sqlite3_free(err);
}
static void sqlog(void* u, int rc, const char* msg) { printf("  log %d: %s\n", rc, msg); }
int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	sqlite3_config(SQLITE_CONFIG_LOG, sqlog, NULL);
	const char* tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	char path[1024];
	snprintf(path, sizeof(path), "%s/sqlprobe4.db", tmp);
	sqlite3* db;
	printf("open -> %d\n", sqlite3_open_v2(path, &db, 0x400006, NULL));
	step(db, "PRAGMA journal_mode");
	step(db, "SELECT count(*) FROM kvs");
	step(db, "DELETE FROM application_identifier_tab WHERE application_identifier_tab.id NOT IN (SELECT DISTINCT application_identifier FROM kvs)");
	step(db, "BEGIN EXCLUSIVE");
	step(db, "COMMIT");
	step(db, "BEGIN IMMEDIATE");
	step(db, "CREATE TABLE IF NOT EXISTS x86code(x)");
	step(db, "COMMIT");
	step(db, "PRAGMA wal_checkpoint(TRUNCATE)");
	sqlite3_close(db);
	return 0;
}
