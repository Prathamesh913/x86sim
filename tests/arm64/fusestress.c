// Speed 4: fuse_svc_ret stress — many svc;ret thunks (libsystem) + nested work that
// exercises return-to-LR after CallSVC. Compare X86SIM_A64_FUSE_SVC=0 vs =1.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>

static volatile uint64_t sink;
static volatile int ready, go, done_workers;
static uint64_t worker_hash[4];

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static uint64_t hammer(int id, int rounds) {
    uint64_t h = 0x9e3779b97f4a7c15ull ^ (uint64_t)id;
    char buf[64];
    for (int i = 0; i < rounds; i++) {
        // Each of these is a guest→native svc;ret thunk under libarm64jit.
        void *p = malloc(32 + (i & 63));
        if (!p) return 0;
        memset(p, (i + id) & 0xff, 16);
        h ^= *(uint64_t *)p;
        h *= 0xbf58476d1ce4e5b9ull;
        snprintf(buf, sizeof buf, "%d:%d:%llx", id, i, (unsigned long long)h);
        h ^= (uint64_t)strlen(buf);
        free(p);
        if ((i & 255) == 0) {
            h ^= (uint64_t)getpid();
            h ^= (uint64_t)time(NULL);
        }
        sink = h;
    }
    return h;
}

static void *worker(void *arg) {
    int id = (int)(intptr_t)arg;
    __sync_fetch_and_add(&ready, 1);
    while (!go) { /* spin */ }
    worker_hash[id] = hammer(id, 8000);
    __sync_fetch_and_add(&done_workers, 1);
    return NULL;
}

int main(void) {
    const char *fuse = getenv("X86SIM_A64_FUSE_SVC");
    printf("fusestress: FUSE_SVC=%s\n", fuse ? fuse : "(unset)");
    uint64_t t0 = now_ns();
    uint64_t solo = hammer(99, 20000);
    uint64_t t1 = now_ns();

    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    while (ready < 4) { /* wait */ }
    go = 1;
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    uint64_t t2 = now_ns();

    uint64_t wh = 0;
    for (int i = 0; i < 4; i++) wh ^= worker_hash[i];
    printf("solo_hash %llx solo_ms %.1f workers_hash %llx workers_ms %.1f sink %llx\n",
           (unsigned long long)solo, (t1 - t0) / 1e6,
           (unsigned long long)wh, (t2 - t1) / 1e6,
           (unsigned long long)sink);
    // Non-zero hashes and completed workers ⇒ fuse did not wedge returns.
    if (!solo || !wh || done_workers != 4) {
        fprintf(stderr, "fusestress FAIL solo=%llx wh=%llx done=%d\n",
                (unsigned long long)solo, (unsigned long long)wh, done_workers);
        return 1;
    }
    printf("fusestress OK\n");
    return 0;
}
