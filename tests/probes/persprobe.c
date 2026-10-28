// Spike 3.3 probe: what the persona syscall returns in a simulator process
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
struct kpersona_info { uint32_t persona_info_version; uid_t persona_id; int persona_type; gid_t persona_gid;
	uint32_t persona_ngroups; gid_t persona_groups[16]; uid_t persona_gmuid; char persona_name[256]; uint32_t persona_reserved[8]; };
extern int kpersona_pidinfo(pid_t pid, struct kpersona_info* info);
extern int kpersona_get(uid_t* id);
extern int kpersona_info(uid_t id, struct kpersona_info* info);
int main(void)
{
	struct kpersona_info info; uid_t id = 0;
	memset(&info, 0, sizeof(info)); info.persona_info_version = 2;
	errno = 0; int r = kpersona_get(&id);
	printf("kpersona_get r=%d errno=%d id=%d\n", r, errno, id);
	errno = 0; r = kpersona_pidinfo(getpid(), &info);
	printf("kpersona_pidinfo r=%d errno=%d id=%d type=%d gid=%d name='%s'\n", r, errno, info.persona_id, info.persona_type, info.persona_gid, info.persona_name);
	fflush(stdout);
	return 0;
}
