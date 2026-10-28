#include <stdio.h>
#include <unistd.h>
#include <sys/event.h>

int main(void) {
    int kq = kqueue();
    printf("kqueue() = %d\n", kq);
    if (kq < 0) { perror("kqueue"); return 1; }

    int p[2];
    pipe(p);
    struct kevent ch[3];
    EV_SET(&ch[0], 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 50, NULL);
    EV_SET(&ch[1], 2, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    EV_SET(&ch[2], p[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kq, ch, 3, NULL, 0, NULL) < 0) { perror("kevent add"); return 1; }

    struct kevent trig;
    EV_SET(&trig, 2, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    kevent(kq, &trig, 1, NULL, 0, NULL);
    write(p[1], "x", 1);

    int seen = 0;
    for (int round = 0; round < 3 && seen != 7; round++) {
        struct kevent ev[4];
        struct timespec ts = { 1, 0 };
        int n = kevent(kq, NULL, 0, ev, 4, &ts);
        for (int i = 0; i < n; i++) {
            printf("event filter=%d ident=%lu\n", ev[i].filter, (unsigned long) ev[i].ident);
            seen |= ev[i].filter == EVFILT_TIMER ? 1 : ev[i].filter == EVFILT_USER ? 2 : 4;
            if (ev[i].filter == EVFILT_READ) { char c; read(p[0], &c, 1); }
        }
    }
    printf(seen == 7 ? "kqtest OK\n" : "kqtest FAILED (seen=%d)\n", seen);
    return seen == 7 ? 0 : 1;
}
