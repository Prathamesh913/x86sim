// x86Code: after 100 s, launchctl print the pid domains (and CollectionsPoster service) of PosterBoard/chronod (sim job)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
int proc_listallpids(void* buf, int size);
int proc_name(int pid, void* buf, unsigned size);
int proc_pidpath(int pid, void* buf, unsigned size);
extern char** environ;
static void lc(const char* target) {
	char* av[] = { "launchctl", "print", (char*)target, NULL };
	pid_t p; int st;
	printf("===== launchctl print %s\n", target);
	int e = posix_spawn(&p, "/bin/launchctl", NULL, NULL, av, environ);
	if (e == 0) waitpid(p, &st, 0); else printf("spawn failed %d\n", e);
}
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	char* d = getenv("LCPROBE_DELAY");
	sleep(d ? atoi(d) : 100);
	int pids[4096]; int n = proc_listallpids(pids, sizeof pids);
	for (int i = 0; i < n; i++) {
		char name[256] = ""; proc_name(pids[i], name, sizeof name); if (!name[0] || !strcmp(name, "mldr")) proc_pidpath(pids[i], name, sizeof name);
		printf("pid %d %s\n", pids[i], name);
		if (strstr(name, "PosterBoard") || strstr(name, "CollectionsPoster")) {
			char t[512]; snprintf(t, sizeof t, "pid/%d", pids[i]); lc(t);
			snprintf(t, sizeof t, "pid/%d/com.apple.WallpaperKit.CollectionsPoster", pids[i]); lc(t);
		}
	}
	return 0;
}
