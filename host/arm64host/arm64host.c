// arm64host: the x86_64 macOS program Darling's mldr runs in place of an arm64-only Mach-O. It starts like any Darling process (native libSystem, TSD, darlingserver), then loads
// the ELF library libarm64jit.so through Darling's elfcalls and hands over: dynarmic runs the arm64 dyld_sim and the
// program; libsystem_kernel/platform/pthread stay this process's native x86 libraries.
// mldr sets X86CODE_A64_EXE (the arm64 program), X86CODE_A64_JIT (the Linux path of libarm64jit.so) and renames
// DYLD_* to X86CODE_A64_DYLD_*. Run by hand: X86CODE_A64_JIT=<so> arm64host <arm64 program> [args...].
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../external/darling/src/startup/mldr/elfcalls/elfcalls.h"

struct darwin_api {
	unsigned version;
	void *(*dlopen)(const char *, int);
	void *(*dlsym)(void *, const char *);
	int (*dladdr)(const void *, Dl_info *);
	int *(*error)(void);
};

int main(int argc, char **argv, char **envp, char **apple)
{
	struct elf_calls *ec = NULL;
	for (int i = 0; apple[i]; i++)
		if (!strncmp(apple[i], "elf_calls=", 10))
			ec = (struct elf_calls *)strtoul(apple[i] + 10, NULL, 16);
	const char *jit = getenv("X86CODE_A64_JIT");
	if (!ec || !jit) {
		fprintf(stderr, "arm64host: needs Darling's elf_calls and X86CODE_A64_JIT\n");
		return 127;
	}
	void *h = ec->dlopen(jit);
	if (!h) {
		fprintf(stderr, "arm64host: cannot load %s: %s\n", jit, ec->dlerror());
		return 127;
	}
	int (*entry)(int, char **, char **, char **, const struct darwin_api *) = ec->dlsym(h, "a64_host_main");
	if (!entry) {
		fprintf(stderr, "arm64host: %s has no a64_host_main\n", jit);
		return 127;
	}
	static const struct darwin_api api = {1, dlopen, dlsym, dladdr, __error};
	return entry(argc, argv, envp, apple, &api);
}
