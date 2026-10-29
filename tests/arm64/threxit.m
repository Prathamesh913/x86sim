// Stage 2 (arm64 plan): threads that end with pthread_exit / [NSThread exit]
#import <Foundation/Foundation.h>
#include <pthread.h>
static void *t1(void *a) { pthread_exit((void *)42); }
int main(void) {
    @autoreleasepool {
        pthread_t th; void *r = 0;
        pthread_create(&th, NULL, t1, NULL); pthread_join(th, &r);
        printf("pthread_exit value %ld (want 42)\n", (long)r);
        __block int ran = 0;
        NSThread *nt = [[NSThread alloc] initWithBlock:^{ ran = 1; [NSThread exit]; }];
        [nt start];
        for (int i = 0; i < 200 && !nt.finished; i++) usleep(10000);
        printf("NSThread exit ran %d finished %d\n", ran, nt.finished);
    }
    return 0;
}
