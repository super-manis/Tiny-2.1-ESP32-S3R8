/* tiny_kernels.c - portable kernels and SIMD dispatch. */
#include "tiny_kernels.h"

int32_t tiny_dot_s8(const int8_t *a, const int8_t *b, int n)
{
#if defined(TINY_HAVE_PIE)
    return tiny_dot_s8_pie(a, b, n);
#else
    /* Four independent accumulators: lets host compilers vectorise and the
     * Xtensa pipeline overlap loads with multiplies. */
    int32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (int i = 0; i < n; i += 4) {
        s0 += (int32_t)a[i + 0] * b[i + 0];
        s1 += (int32_t)a[i + 1] * b[i + 1];
        s2 += (int32_t)a[i + 2] * b[i + 2];
        s3 += (int32_t)a[i + 3] * b[i + 3];
    }
    return s0 + s1 + s2 + s3;
#endif
}
