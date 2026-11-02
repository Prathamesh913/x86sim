// Speed 32: mach_msg cost. Two threads ping-pong a simple (no rights beyond the reply, no OOL) message.
// Mode "rt": client does send|rcv on a reply port (MACH_MSG_TYPE_MAKE_SEND_ONCE), server receives then sends the reply.
// Mode "oneway": separate send and receive calls on two plain ports.
// Usage: msgbench.sim [iterations] [payload bytes] [trap]
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

typedef struct { mach_msg_header_t h; char body[1024]; mach_msg_trailer_t t; } msg_t;
static mach_port_t srv, a2b, b2a;
static int N = 20000, PAY = 32;

static void *server(void *x) {
  msg_t m;
  for (int i = 0; i < N; i++) {
    memset(&m.h, 0, sizeof m.h);
    if (mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof m, srv, 0, 0)) { puts("srv rcv fail"); exit(1); }
    mach_port_t r = m.h.msgh_remote_port;
    m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    m.h.msgh_remote_port = r; m.h.msgh_local_port = 0; m.h.msgh_size = sizeof m.h + PAY;
    if (mach_msg(&m.h, MACH_SEND_MSG, m.h.msgh_size, 0, 0, 0, 0)) { puts("srv snd fail"); exit(1); }
  }
  return 0;
}
static void *echo(void *x) {
  msg_t m;
  for (int i = 0; i < N; i++) {
    memset(&m.h, 0, sizeof m.h);
    if (mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof m, a2b, 0, 0)) { puts("echo rcv fail"); exit(1); }
    m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    m.h.msgh_remote_port = b2a; m.h.msgh_local_port = 0; m.h.msgh_size = sizeof m.h + PAY;
    if (mach_msg(&m.h, MACH_SEND_MSG, m.h.msgh_size, 0, 0, 0, 0)) { puts("echo snd fail"); exit(1); }
  }
  return 0;
}
static double now(void) {
  static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
  return (double)mach_absolute_time() * tb.numer / tb.denom;
}
static void ru(struct rusage *r) { getrusage(RUSAGE_SELF, r); }

int main(int argc, char **argv) {
  if (argc > 1) N = atoi(argv[1]);
  if (argc > 2) PAY = atoi(argv[2]);
  mach_port_t me = mach_task_self();
  mach_port_allocate(me, MACH_PORT_RIGHT_RECEIVE, &srv);
  mach_port_insert_right(me, srv, srv, MACH_MSG_TYPE_MAKE_SEND);
  mach_port_t reply; mach_port_allocate(me, MACH_PORT_RIGHT_RECEIVE, &reply);
  mach_port_allocate(me, MACH_PORT_RIGHT_RECEIVE, &a2b);
  mach_port_allocate(me, MACH_PORT_RIGHT_RECEIVE, &b2a);
  pthread_t t; struct rusage r0, r1; msg_t m;
  if (argc > 3 && !strcmp(argv[3], "trap")) { // only the null trap, for profiling
    double a = now(); for (int i = 0; i < N; i++) mach_port_type(me, a2b, &(mach_port_type_t){0});
    printf("null trap: %.2f us\n", (now() - a) / N / 1e3); return 0;
  }

  pthread_create(&t, 0, server, 0);
  ru(&r0); double t0 = now();
  for (int i = 0; i < N; i++) {
    memset(&m.h, 0, sizeof m.h);
    m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    m.h.msgh_remote_port = srv; m.h.msgh_local_port = reply; m.h.msgh_size = sizeof m.h + PAY;
    if (mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG, m.h.msgh_size, sizeof m, reply, 0, 0)) { puts("cli fail"); return 1; }
  }
  double t1 = now(); ru(&r1); pthread_join(t, 0);
  printf("rt: %d round trips, %.2f us each, vcsw %ld ivcsw %ld per rt %.2f, cpu %.2f us/rt\n", N, (t1 - t0) / N / 1e3,
         r1.ru_nvcsw - r0.ru_nvcsw, r1.ru_nivcsw - r0.ru_nivcsw, (double)(r1.ru_nvcsw - r0.ru_nvcsw) / N,
         ((r1.ru_utime.tv_sec - r0.ru_utime.tv_sec + r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) * 1e6 +
          (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec + r1.ru_stime.tv_usec - r0.ru_stime.tv_usec)) / N);

  pthread_create(&t, 0, echo, 0);
  ru(&r0); t0 = now();
  for (int i = 0; i < N; i++) {
    memset(&m.h, 0, sizeof m.h);
    m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    m.h.msgh_remote_port = a2b; m.h.msgh_size = sizeof m.h + PAY;
    if (mach_msg(&m.h, MACH_SEND_MSG, m.h.msgh_size, 0, 0, 0, 0)) { puts("snd fail"); return 1; }
    memset(&m.h, 0, sizeof m.h);
    if (mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof m, b2a, 0, 0)) { puts("rcv fail"); return 1; }
  }
  t1 = now(); ru(&r1); pthread_join(t, 0);
  printf("oneway: %d ping-pongs, %.2f us each (2 msgs, 4 calls)\n", N, (t1 - t0) / N / 1e3);

  // send to self, no wakeup: the pure call cost
  t0 = now();
  for (int i = 0; i < N; i++) {
    memset(&m.h, 0, sizeof m.h);
    m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    m.h.msgh_remote_port = a2b; m.h.msgh_size = sizeof m.h + PAY;
    mach_msg(&m.h, MACH_SEND_MSG, m.h.msgh_size, 0, 0, 0, 0);
    mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof m, a2b, 0, 0);
  }
  t1 = now();
  printf("self: %.2f us per send+rcv (no wakeup)\n", (t1 - t0) / N / 1e3);
  t0 = now();
  for (int i = 0; i < N; i++) mach_port_type(me, a2b, &(mach_port_type_t){0});
  t1 = now();
  printf("null trap (mach_port_type): %.2f us\n", (t1 - t0) / N / 1e3);
  return 0;
}
