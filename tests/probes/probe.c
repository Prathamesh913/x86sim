// Spike 3.3 probe: user database and path resolution inside a simulator process
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

int main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "/";
	char buf[PATH_MAX];

	errno = 0;
	struct passwd* pw = getpwuid(getuid());
	printf("uid %d: getpwuid -> %s (home %s) errno=%d\n", getuid(), pw ? pw->pw_name : "(null)", pw ? pw->pw_dir : "-", errno);

	errno = 0;
	char* r = realpath(path, buf);
	printf("realpath(%s) -> %s errno=%d (%s)\n", path, r ? r : "(null)", errno, strerror(errno));

	int fd = open(path, O_RDONLY);
	errno = 0;
	int rc = fd >= 0 ? fcntl(fd, F_GETPATH, buf) : -1;
	printf("open fd=%d, F_GETPATH rc=%d -> %s errno=%d\n", fd, rc, rc == 0 ? buf : "-", errno);
	return 0;
}
