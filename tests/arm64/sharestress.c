// Stage 3: multi-thread translation stress for the shared code cache.
// Many threads concurrently translate and execute overlapping call graphs (indirect
// calls + a mutex-protected shared counter) so block linking / fast-dispatch races
// show up. Expect: exit 0 under X86SIM_A64_SHARED=1 (BlockLinking on by default via atomic
// patch heads). X86SIM_A64_LINK=0 disables linking for experiments.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

#define NT 16
#define ROUNDS 8000

static atomic_long hits;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static long guarded;

typedef long (*fn_t)(long);

static long f0(long x) { return x + 1; }
static long f1(long x) { return x * 3 + 1; }
static long f2(long x) { return (x ^ 0x5a5a5a5a) + 7; }
static long f3(long x) { return x - (x >> 3); }
static long f4(long x) { return ~x + 13; }
static long f5(long x) { return (x << 1) ^ (x >> 1); }
static long f6(long x) { return x ? (x % 97) + 2 : 3; }
static long f7(long x) { return x * x + x + 1; }

static fn_t table[8] = {f0, f1, f2, f3, f4, f5, f6, f7};

static void *worker(void *arg) {
    long id = (long)arg;
    long acc = id + 1;
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < 8; i++) {
            fn_t f = table[(id + r + i) & 7];
            acc = f(acc);
            // Indirect through a volatile pointer so the JIT cannot fold the call.
            volatile fn_t vf = table[(acc + i) & 7];
            acc = vf(acc);
        }
        if ((r & 63) == 0) {
            pthread_mutex_lock(&mu);
            guarded += acc & 1;
            pthread_mutex_unlock(&mu);
        }
        atomic_fetch_add(&hits, 1);
    }
    return (void *)(acc | 1); // non-zero
}

int main(void) {
    pthread_t t[NT];
    long sum = 0;
    for (long i = 0; i < NT; i++) {
        if (pthread_create(&t[i], NULL, worker, (void *)i) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 2;
        }
    }
    for (int i = 0; i < NT; i++) {
        void *r;
        pthread_join(t[i], &r);
        sum += (long)r;
    }
    long want_hits = (long)NT * ROUNDS;
    printf("hits %ld (want %ld) guarded %ld joins_nonzero %d\n",
           (long)hits, want_hits, guarded, sum != 0);
    if (hits != want_hits) return 1;
    if (sum == 0) return 1;
    return 0;
}
