// Stage 2 (arm64 plan): FEAT_FP16 Advanced SIMD (executed by libarm64jit's fp16.cpp): compare the hash with the Mac
#include <arm_neon.h>
#include <stdio.h>
#include <string.h>
static unsigned h = 0;
static void mix(float16x8_t v) { unsigned short u[8]; memcpy(u, &v, 16); for (int i = 0; i < 8; i++) h = h * 31 + u[i]; }
static void mixu(uint16x8_t v) { unsigned short u[8]; memcpy(u, &v, 16); for (int i = 0; i < 8; i++) h = h * 31 + u[i]; }
int main(void) {
    volatile float16_t src[16] = {1.5, -2.25, 0.1, 3.0, -0.5, 7.75, 100.0, -0.0, 2.0, 0.25, -8.0, 1e-3, 5.5, -3.5, 0.75, 9.0};
    float16x8_t a = vld1q_f16((const float16_t *)src), b = vld1q_f16((const float16_t *)src + 8);
    mix(vaddq_f16(a, b)); mix(vsubq_f16(a, b)); mix(vmulq_f16(a, b)); mix(vdivq_f16(a, b));
    mix(vfmaq_f16(a, a, b)); mix(vfmsq_f16(a, a, b)); mix(vfmaq_laneq_f16(a, a, b, 3)); mix(vmulq_laneq_f16(a, b, 5));
    mix(vmaxq_f16(a, b)); mix(vminq_f16(a, b)); mix(vmaxnmq_f16(a, b)); mix(vminnmq_f16(a, b)); mix(vabdq_f16(a, b));
    mix(vpaddq_f16(a, b)); mix(vpmaxq_f16(a, b)); mix(vpminq_f16(a, b));
    mixu(vcgtq_f16(a, b)); mixu(vcgeq_f16(a, b)); mixu(vceqq_f16(a, b)); mixu(vcagtq_f16(a, b));
    mix(vrecpsq_f16(a, b)); mix(vrsqrtsq_f16(a, b)); mix(vabsq_f16(a)); mix(vnegq_f16(a)); mix(vsqrtq_f16(vabsq_f16(b)));
    mix(vrndmq_f16(a)); mix(vrndnq_f16(b)); mix(vrndaq_f16(a)); mixu(vcvtq_u16_f16(vabsq_f16(b))); mixu(vreinterpretq_u16_s16(vcvtq_s16_f16(a)));
    mix(vcvtq_f16_u16(vdupq_n_u16(1234))); mix(vcvtq_f16_s16(vdupq_n_s16(-77)));
    float16x4_t c = vget_low_f16(a), d = vget_high_f16(b);
    mix(vcombine_f16(vfma_lane_f16(c, c, d, 1), vmul_f16(c, d)));
    printf("fp16v hash %08x maxv %g\n", h, (double)vmaxvq_f16(b));
    return 0;
}
