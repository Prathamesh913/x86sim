// Stage 2 (arm64 plan): FEAT_FP16 scalar arithmetic/compares (dynarmic translates them via binary32)
#include <stdio.h>
int main(void) {
    volatile _Float16 a = 1.5f16, b = -2.25f16, c = 0.1f16;
    unsigned h = 0;
    _Float16 r[] = {a + b, a * b, a / c, b - c, __builtin_sqrtf16(a), a > b ? a : b, a < c ? a : c, a * b + c};
    for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++) { unsigned short u; __builtin_memcpy(&u, &r[i], 2); h = h * 31 + u; printf("%g ", (double)r[i]); }
    printf("\nhash %08x cmp %d %d\n", h, a > b, c >= a);
    return 0;
}
