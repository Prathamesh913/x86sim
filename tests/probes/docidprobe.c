// x86Code probe: document ids as GenerationalStorage resolves them (GSLibraryResolveDocumentId2): getattrlist
// ATTR_CMN_DOCUMENT_ID of a path, openbyid_np(fsid, docid), then fgetattrlist(FLAGS | DOCUMENT_ID) on the result.
//   run-sim.sh tests/probes/docidprobe <path>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/attr.h>
#include <sys/mount.h>
extern int openbyid_np(fsid_t* fsid, fsobj_id_t* objid, int flags);

int main(int argc, char** argv) {
	struct attrlist al = { ATTR_BIT_MAP_COUNT, 0, ATTR_CMN_FSID | ATTR_CMN_FILEID | ATTR_CMN_FLAGS | ATTR_CMN_DOCUMENT_ID };
	struct __attribute__((packed)) { uint32_t len; fsid_t fsid; uint32_t flags; uint32_t docid; uint64_t fileid; } a;  // attribute order
	memset(&a, 0, sizeof a);
	int r = getattrlist(argv[1], &al, &a, sizeof a, 0);
	printf("getattrlist %d errno %d: len %u fsid %d:%d fileid %llu flags %#x docid %u\n", r, r ? errno : 0, a.len,
		a.fsid.val[0], a.fsid.val[1], (unsigned long long) a.fileid, a.flags, a.docid);
	uint64_t id = a.docid;
	fsid_t fsid = a.fsid;
	int fd = openbyid_np(&fsid, (fsobj_id_t*) &id, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	printf("openbyid_np(docid) = %d errno %d\n", fd, fd < 0 ? errno : 0);
	if (fd >= 0) {
		char p[1024]; fcntl(fd, F_GETPATH, p); printf("  path %s\n", p);
		struct attrlist al2 = { ATTR_BIT_MAP_COUNT, 0, ATTR_CMN_FLAGS | ATTR_CMN_DOCUMENT_ID };
		struct { uint32_t len, flags, docid; } b = { 0 };
		r = fgetattrlist(fd, &al2, &b, 12, 0x20);
		printf("fgetattrlist(0x20) %d errno %d: len %u flags %#x docid %u\n", r, r ? errno : 0, b.len, b.flags, b.docid);
	}
	struct statfs sf;
	if (statfs(argv[1], &sf) == 0) {
		printf("statfs f_fsid %d:%d f_fstypename %s mnton %s\n", sf.f_fsid.val[0], sf.f_fsid.val[1], sf.f_fstypename, sf.f_mntonname);
		fd = openbyid_np(&sf.f_fsid, (fsobj_id_t*) &id, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
		printf("openbyid_np(statfs fsid, docid) = %d errno %d\n", fd, fd < 0 ? errno : 0);
	}
	return 0;
}
