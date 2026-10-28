// Spike 3.3 probe: what proc_pidpath/proc_name report for this process (run as a launchd job)
extern int proc_pidpath(int pid, void* buf, unsigned int size);
extern int proc_name(int pid, void* buf, unsigned int size);
#include <stdio.h>
#include <unistd.h>
int main(void)
{
	char path[4096] = "", name[256] = "";
	int r = proc_pidpath(getpid(), path, sizeof(path));
	proc_name(getpid(), name, sizeof(name));
	printf("pid %d proc_pidpath r=%d '%s' proc_name '%s'\n", getpid(), r, path, name);
	fflush(stdout);
	return 0;
}
