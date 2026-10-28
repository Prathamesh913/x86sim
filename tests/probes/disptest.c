#include <stdio.h>
#include <unistd.h>
#include <dispatch/dispatch.h>

int main(void) {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);

    dispatch_async(q, ^{ printf("async on global queue: ok\n"); dispatch_semaphore_signal(done); });
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) { printf("async: TIMEOUT\n"); return 1; }

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), q, ^{ printf("dispatch_after: ok\n"); dispatch_semaphore_signal(done); });
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) { printf("dispatch_after: TIMEOUT\n"); return 1; }

    int p[2];
    pipe(p);
    int rfd = p[0];
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, p[0], 0, q);
    dispatch_source_set_event_handler(src, ^{
        char c; read(rfd, &c, 1);
        printf("read source: got '%c'\n", c);
        dispatch_source_cancel(src);
        dispatch_semaphore_signal(done);
    });
    dispatch_resume(src);
    write(p[1], "x", 1);
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) { printf("read source: TIMEOUT\n"); return 1; }

    printf("disptest OK\n");
    return 0;
}
