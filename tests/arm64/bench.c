// Stage 0 test (b): CPU micro-benchmarks. Same source for the iOS-simulator arm64, macOS arm64 and Linux x86_64 builds.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
static uint64_t now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
#else
#include <time.h>
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ull + t.tv_nsec; }
#endif

#define N_BUF (16u << 20)
static volatile uint64_t sink;

__attribute__((noinline)) static uint64_t b_int(void) {          // integer ALU, multiplies, shifts, data-dependent branches
    uint64_t x = 88172645463325252ull, acc = 0;
    for (uint32_t i = 0; i < 100000000u; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        acc += (x & 1) ? (x >> 3) * 5 : (x % 7);
    }
    return acc;
}
__attribute__((noinline)) static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static uint64_t b_calls(void) { return fib(32); }                   // call/return heavy (return-stack prediction)

typedef uint64_t (*op_t)(uint64_t);
__attribute__((noinline)) static uint64_t op0(uint64_t v) { return v * 3 + 1; }
__attribute__((noinline)) static uint64_t op1(uint64_t v) { return v ^ (v >> 5); }
__attribute__((noinline)) static uint64_t op2(uint64_t v) { return v + 0x9e3779b97f4a7c15ull; }
__attribute__((noinline)) static uint64_t op3(uint64_t v) { return (v << 7) | (v >> 57); }
static op_t ops[4] = { op0, op1, op2, op3 };
__attribute__((noinline)) static uint64_t b_indirect(void) {     // indirect calls (objc_msgSend-like dispatch)
    uint64_t v = 1;
    for (uint32_t i = 0; i < 30000000u; i++) v = ops[(v ^ i) & 3](v);
    return v;
}
static uint8_t *src, *dst;
__attribute__((noinline)) static uint64_t b_memcpy(void) {       // libsystem_platform memcpy (a native thunk under the JIT)
    for (int r = 0; r < 40; r++) { memcpy(dst, src, N_BUF); src[r] ^= dst[N_BUF - 1 - r]; }
    return dst[12345] + dst[N_BUF / 2];
}
__attribute__((noinline)) static uint64_t b_copyloop(void) {     // the same copy as guest code: loads/stores through fastmem
    for (int r = 0; r < 10; r++) {
        uint64_t *d = (uint64_t *)dst, *s = (uint64_t *)src;
        for (uint32_t i = 0; i < N_BUF / 8; i++) d[i] = s[i] + r;
    }
    return dst[777];
}
static void isort(int *a, int n) { for (int i = 1; i < n; i++) { int v = a[i], j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; } }
static void qs(int *a, int n) {
    while (n > 24) {
        int p = a[n / 2], i = 0, j = n - 1;
        while (i <= j) { while (a[i] < p) i++; while (a[j] > p) j--; if (i <= j) { int t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; } }
        if (j + 1 < n - i) { qs(a, j + 1); a += i; n -= i; } else { qs(a + i, n - i); n = j + 1; }
    }
    isort(a, n);
}
__attribute__((noinline)) static uint64_t b_sort(void) {
    int n = 2000000; int *a = (int *)dst; uint32_t x = 12345;
    for (int i = 0; i < n; i++) { x = x * 1103515245u + 12345u; a[i] = (int)(x >> 1); }
    qs(a, n);
    for (int i = 1; i < n; i++) if (a[i - 1] > a[i]) return 0;
    return (uint64_t)a[n / 2];
}
__attribute__((noinline)) static uint64_t b_fp(void) {           // scalar double: mandelbrot
    int count = 0;
    for (int py = 0; py < 600; py++) for (int px = 0; px < 600; px++) {
        double cr = px / 300.0 - 2.0, ci = py / 300.0 - 1.0, zr = 0, zi = 0; int k = 0;
        while (k < 200 && zr * zr + zi * zi < 4.0) { double t = zr * zr - zi * zi + cr; zi = 2 * zr * zi + ci; zr = t; k++; }
        count += k;
    }
    return count;
}
__attribute__((noinline)) static uint64_t b_simd(void) {         // auto-vectorized float (NEON on arm64, SSE on x86)
    float *a = (float *)src, *b = (float *)dst; int n = 1 << 20;
    for (int i = 0; i < n; i++) { a[i] = (float)(i & 1023) * 0.5f; b[i] = (float)(i % 517) * 0.25f; }
    float acc = 0;
    for (int r = 0; r < 100; r++) {
        float s = 0;
        for (int i = 0; i < n; i++) { b[i] = b[i] * 0.999f + a[i]; s += a[i] * b[i]; }
        acc += s * 1e-9f;
    }
    return (uint64_t)acc;
}
struct bench { const char *name; uint64_t (*fn)(void); };
int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 3;
    src = malloc(N_BUF); dst = malloc(N_BUF);
    for (uint32_t i = 0; i < N_BUF; i++) src[i] = (uint8_t)(i * 31);
    struct bench bs[] = { {"int", b_int}, {"calls", b_calls}, {"indirect", b_indirect}, {"memcpy", b_memcpy},
                          {"copyloop", b_copyloop}, {"sort", b_sort}, {"fp", b_fp}, {"simd", b_simd} };
    uint64_t total = 0;
    for (unsigned b = 0; b < sizeof bs / sizeof bs[0]; b++) {
        uint64_t best = ~0ull, first = 0, res = 0;
        for (int r = 0; r < reps; r++) {
            uint64_t t0 = now_ns(); res = bs[b].fn(); uint64_t dt = now_ns() - t0;
            if (r == 0) first = dt;
            if (dt < best) best = dt;
        }
        total += best;
        printf("%-9s best %8.2f ms  first %8.2f ms  result %llu\n", bs[b].name, best / 1e6, first / 1e6, (unsigned long long)res);
    }
    printf("total(best) %.2f ms\n", total / 1e6);
    return 0;
}
