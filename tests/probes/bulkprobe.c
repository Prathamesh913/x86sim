// Spike 3.3 probe: getattrlistbulk on a directory (name + object type per entry)
#include <sys/attr.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
int main(int argc, char** argv)
{
	struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT,
		.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_OBJTYPE };
	char buf[256]; // small on purpose: several calls
	int fd = open(argc > 1 ? argv[1] : "/", O_RDONLY | O_DIRECTORY), total = 0, n;
	while ((n = getattrlistbulk(fd, &al, buf, sizeof(buf), 0)) > 0) {
		char* p = buf;
		for (int i = 0; i < n; i++) {
			uint32_t len = *(uint32_t*) p;
			attribute_set_t* ret = (attribute_set_t*) (p + 4);
			attrreference_t* nref = (attrreference_t*) (p + 4 + sizeof(attribute_set_t));
			uint32_t type = *(uint32_t*) ((char*) nref + sizeof(attrreference_t));
			printf("  %-30s type %u ret 0x%x len %u\n", (char*) nref + nref->attr_dataoffset, type, ret->commonattr, len);
			p += len; total++;
		}
	}
	printf("total %d, last n=%d errno=%d\n", total, n, errno);
	return 0;
}
