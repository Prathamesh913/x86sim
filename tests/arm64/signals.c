// Stage 2 (arm64 plan): async signal delivery to guest handlers with an arm64 ucontext.
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ucontext.h>

static volatile int usr1, alrm, bad;
static volatile unsigned long long seen_pc;

static void on_usr1(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    if (sig != SIGUSR1 || si->si_signo != SIGUSR1 || !uc || !uc->uc_mcontext) bad++;
    else seen_pc = uc->uc_mcontext->__ss.__pc;
    usr1++;
}
static void on_alrm(int sig) { if (sig != SIGALRM) bad++; alrm++; }

int main(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_usr1; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGALRM, on_alrm);
    raise(SIGUSR1);
    kill(getpid(), SIGUSR1);
    // timer signals during pure compute (delivered at a block boundary)
    struct itimerval it = {{0, 2000}, {0, 2000}};
    setitimer(ITIMER_REAL, &it, NULL);
    volatile unsigned long long x = 1;
    for (unsigned long long i = 0; i < 400000000ull && alrm < 20; i++) x = x * 6364136223846793005ull + 1;
    struct itimerval off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &off, NULL);
    // sigsuspend wakes up for a pending signal
    sigset_t m, old; sigemptyset(&m); sigaddset(&m, SIGUSR1);
    sigprocmask(SIG_BLOCK, &m, &old);
    kill(getpid(), SIGUSR1);
    sigset_t w; sigemptyset(&w);
    sigsuspend(&w);
    sigprocmask(SIG_SETMASK, &old, NULL);
    struct sigaction q; sigaction(SIGUSR1, NULL, &q);
    printf("usr1 %d (want 3) alrm %d (want >= 20) bad %d pc %s handler-query %s\n", usr1, alrm, bad,
           seen_pc ? "ok" : "zero", q.sa_sigaction == on_usr1 && (q.sa_flags & SA_SIGINFO) ? "ok" : "wrong");
    return usr1 == 3 && alrm >= 20 && !bad ? 0 : 1;
}
