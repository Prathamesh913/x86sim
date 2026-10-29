// Stage 0 test (d): pthreads + C11 atomics (LSE ldadd/cas/swp on arm64) + a mutex.
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#define NT 4
#define ITERS 200000
static _Atomic long counter, cas_counter; static _Atomic int flag;
static long locked; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < ITERS; i++) {
        atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed);
        long v = atomic_load(&cas_counter);
        while (!atomic_compare_exchange_weak(&cas_counter, &v, v + 2)) {}
        if ((i & 1023) == 0) { pthread_mutex_lock(&mu); locked += id; pthread_mutex_unlock(&mu); }
    }
    atomic_exchange(&flag, (int)id);
    return (void *)(id * 10);
}
int main(void) {
    pthread_t t[NT]; long sum = 0;
    for (long i = 0; i < NT; i++) pthread_create(&t[i], NULL, worker, (void *)(i + 1));
    for (int i = 0; i < NT; i++) { void *r; pthread_join(t[i], &r); sum += (long)r; }
    long want_locked = 0; for (int i = 1; i <= NT; i++) want_locked += i * ((ITERS + 1023) / 1024);
    printf("counter %ld (want %d) cas %ld (want %d) locked %ld (want %ld) joins %ld (want 100) flag %d\n",
           (long)counter, NT * ITERS, (long)cas_counter, 2 * NT * ITERS, locked, want_locked, sum, (int)flag);
    return !(counter == NT * ITERS && cas_counter == 2 * NT * ITERS && locked == want_locked && sum == 100);
}
