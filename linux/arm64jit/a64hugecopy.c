// a64hugecopy MAP HP (Speed 35): builds the huge-page tmpfs copy HP of the shared relocated AOT image MAP
// (dynarmic a64_emit_x64.cpp HugeImageFd) in its own Linux process. libarm64jit spawns it (setsid, nice 10) from the
// first guest process that finds no copy, and that process and every one after it map MAP itself until the copy is
// linked in, so no guest's open waits for the 2-5 s copy and no guest thread does the work (Speed 34's copy thread
// inside UserEventAgent died after ~350 MB). Marker HP.building (O_EXCL, our pid; one older than 300 s or of a dead pid
// is taken over); copies of other layouts in HP's directory are dropped; a copy that gets mostly 4 KB pages is not kept
// and HP.small tells later processes not to try again this boot (boot-a64.sh clears it). Log: <dir>/a64hugecopy.log.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FILE *lg;
static void logf_(const char *fmt, const char *a, unsigned long long x, unsigned long long y) {
    if (!lg) return;
    fprintf(lg, "%ld pid %d: ", (long)time(NULL), getpid());
    fprintf(lg, fmt, a, x, y);
    fputc('\n', lg);
    fflush(lg);
}

static int take_marker(const char *mk) {
    for (int tries = 0; tries < 2; tries++) {
        int k = open(mk, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (k >= 0) {
            char me[32];
            int n = snprintf(me, sizeof me, "%d", getpid());
            if (write(k, me, n) != n) { close(k); unlink(mk); return 0; }
            close(k);
            return 1;
        }
        char buf[32] = {0};
        int r = open(mk, O_RDONLY | O_CLOEXEC);
        ssize_t n = r >= 0 ? read(r, buf, sizeof buf - 1) : -1;
        if (r >= 0) close(r);
        pid_t owner = n > 0 ? (pid_t)atoi(buf) : 0;
        int alive = owner > 0 && (kill(owner, 0) == 0 || errno == EPERM);
        struct stat ks;
        if (alive && stat(mk, &ks) == 0 && time(NULL) - ks.st_mtime < 300) return 0;
        unlink(mk);
    }
    return 0;
}

int main(int argc, char **argv) {
    // Perf 12: optional PAD: bytes before the copy of MAP, so HP's offsets match the image's addresses modulo 2 MB
    if (argc != 3 && argc != 4) { fprintf(stderr, "usage: a64hugecopy MAP HP [PAD]\n"); return 2; }
    const unsigned long long pad = argc == 4 ? strtoull(argv[3], NULL, 10) : 0;
    // Guests run with PR_SET_THP_DISABLE (host.cpp, RAM 1), which a child inherits: it also kept this copy off
    // 2 MB pages (every boot logged "mostly small pages"). The copy is the one place huge pages are wanted.
    prctl(PR_SET_THP_DISABLE, 0, 0, 0, 0);
    const char *map = argv[1], *hp = argv[2];
    char dir[4096], mk[4200], small[4200], tmp[4200], lp[4200];
    snprintf(dir, sizeof dir, "%s", hp);
    char *sl = strrchr(dir, '/');
    if (!sl) return 2;
    *sl = 0;
    const char *base = sl + 1 - dir + hp;
    snprintf(lp, sizeof lp, "%s/a64hugecopy.log", dir);
    lg = fopen(lp, "a");
    snprintf(mk, sizeof mk, "%s.building", hp);
    snprintf(small, sizeof small, "%s.small", hp);
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", hp, getpid());
    int mfd = open(map, O_RDONLY | O_CLOEXEC);
    struct stat ms, hs;
    if (mfd < 0 || fstat(mfd, &ms) != 0 || ms.st_size <= 0) { logf_("no map %s%llu%llu", map, 0, 0); return 1; }
    const unsigned long long len = (unsigned long long)ms.st_size + pad;
    if (access(small, F_OK) == 0) return 0;
    if (stat(hp, &hs) == 0 && (unsigned long long)hs.st_size == len) return 0;
    // detach: libarm64jit waits for this process (a clone child without SIGCHLD); the copy runs in a grandchild
    pid_t c = fork();
    if (c < 0) return 1;
    if (c > 0) _exit(0);
    setsid();
    if (nice(10) == -1) {}
    if (!take_marker(mk)) return 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    // other layouts' copies (mapped ones stay alive until their processes exit)
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp))) {
            const char *n = de->d_name;
            if (strcmp(n, ".") && strcmp(n, "..") && strncmp(n, base, strlen(base)) && strstr(n, ".map")) unlinkat(dirfd(dp), n, 0);
        }
        closedir(dp);
    }
    int ok = 0;
    unsigned long long pmd_kb = 0, done = 0;
    int w = open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (w >= 0 && ftruncate(w, (off_t)len) == 0) {
        unsigned char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, w, 0);
        if (m != MAP_FAILED) {
            madvise(m, len, MADV_HUGEPAGE);
            done = pad;
            while (done < len) {
                size_t want = len - done < (64ull << 20) ? len - done : (64ull << 20);
                ssize_t n = pread(mfd, m + done, want, (off_t)(done - pad));
                if (n <= 0) break;
                done += (unsigned long long)n;
            }
            ok = done == len;
#ifdef MADV_COLLAPSE
            if (ok) madvise(m, len, MADV_COLLAPSE);
#endif
            if (ok) {
                FILE *sm = fopen("/proc/self/smaps", "r");
                char line[512];
                int in = 0;
                while (sm && fgets(line, sizeof line, sm)) {
                    unsigned long a, b;
                    if (sscanf(line, "%lx-%lx ", &a, &b) == 2) in = a == (unsigned long)m;
                    else if (in && sscanf(line, "ShmemPmdMapped: %lu", &a) == 1) pmd_kb = a;
                }
                if (sm) fclose(sm);
                if (pmd_kb * 1024 * 2 < len) {
                    ok = 0;
                    int s = open(small, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
                    if (s >= 0) close(s);
                }
            }
            munmap(m, len);
        }
    }
    if (w >= 0) close(w);
    if (ok && link(tmp, hp) != 0 && errno != EEXIST) ok = 0;
    unlink(tmp);
    unlink(mk);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    unsigned long long msec = (unsigned long long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
    if (ok) logf_("copied %s: %llu MB on 2 MB pages, %llu ms", hp, pmd_kb >> 10, msec);
    else logf_("no copy %s (copied %llu bytes, mostly small pages or failed), %llu ms", hp, done, msec);
    return ok ? 0 : 1;
}
