// Stage 2 (arm64 plan): threads that end with pthread_exit / [NSThread exit]
#import <Foundation/Foundation.h>
#include <pthread.h>
static void *t1(void *a) { pthread_exit((void *)42); }
int main(void) {
    @autoreleasepool {
        pthread_t th; void *r = 0;
        pthread_create(&th, NULL, t1, NULL); pthread_join(th, &r);
        fprintf(stderr, "pthread_exit value %ld (want 42)\n", (long)r);
        pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &at, t1, NULL); usleep(200000);
        fprintf(stderr, "detached pthread_exit ok\n");
        __block int plain = 0;
        NSThread *pt = [[NSThread alloc] initWithBlock:^{ plain = 1; }];
        [pt start];
        for (int i = 0; i < 200 && !pt.finished; i++) usleep(10000);
        fprintf(stderr, "plain NSThread ran %d finished %d\n", plain, pt.finished);
        __block int ran = 0;
        NSThread *nt = [[NSThread alloc] initWithBlock:^{ ran = 1; [NSThread exit]; }];
        [nt start];
        for (int i = 0; i < 200 && !nt.finished; i++) usleep(10000);
        fprintf(stderr, "NSThread exit ran %d finished %d\n", ran, nt.finished);
    }
    return 0;
}
