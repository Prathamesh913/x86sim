// Stage 3: shared-cache IC invalidate stress.
// Many threads execute overlapping code while others fire IC IALLU / IVAU (dyld/objc style).
// Before the shared ClearCache fix, IALLU walked every block under the process mutex and left
// block_ranges growing — under load this corrupts or stalls; after the fix this must exit 0.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#define NT 12
#define ROUNDS 4000
#define IALLU_EVERY 200

static atomic_long hits;
static atomic_long iallus;
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

static void ic_iallu(void) {
    // Instruction cache invalidate-all to Point of Unification (guest IC op → ClearCache).
    __asm__ __volatile__("ic iallu" ::: "memory");
    __asm__ __volatile__("dsb ish" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
    atomic_fetch_add(&iallus, 1);
}

static void ic_ivau(void *p) {
    __asm__ __volatile__("ic ivau, %0" : : "r"(p) : "memory");
    __asm__ __volatile__("dsb ish" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}

static void *worker(void *arg) {
    long id = (long)arg;
    long acc = id + 1;
    // A few distinct instruction bytes so IVAU has something to target.
    unsigned char bump = (unsigned char)(id & 0xff);
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < 8; i++) {
            fn_t f = table[(id + r + i) & 7];
            acc = f(acc);
            volatile fn_t vf = table[(acc + i) & 7];
            acc = vf(acc);
        }
        if ((r % IALLU_EVERY) == (int)(id % IALLU_EVERY)) {
            ic_iallu();
        }
        if ((r & 31) == 0) {
            ic_ivau(&bump);
            bump++;
        }
        if ((r & 63) == 0) {
            pthread_mutex_lock(&mu);
            guarded += acc & 1;
            pthread_mutex_unlock(&mu);
        }
        atomic_fetch_add(&hits, 1);
    }
    return (void *)(acc | 1);
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
    long want = (long)NT * ROUNDS;
    printf("hits %ld (want %ld) iallus %ld guarded %ld joins_nonzero %d\n",
           (long)hits, want, (long)iallus, guarded, sum != 0);
    if (hits != want) return 1;
    if (sum == 0) return 1;
    if (iallus < 10) return 1;
    return 0;
}
