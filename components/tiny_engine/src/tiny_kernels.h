/* tiny_kernels.h - inner loops. The ESP32-S3 SIMD versions live in
 * tiny_kernels_s3.S; everything else is portable C. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sum a[i]*b[i] for int8 vectors. n must be a positive multiple of 16; b
 * must be 16-byte aligned. a is 16-byte aligned for tables in RAM and
 * unaligned for in-place tables (the PIE kernel handles both). */
int32_t tiny_dot_s8(const int8_t *a, const int8_t *b, int n);

/* Same for any n / alignment (short attention heads). */
static inline int32_t tiny_dot_s8_any(const int8_t *a, const int8_t *b, int n)
{
    int32_t s = 0;
    for (int i = 0; i < n; i++)
        s += (int32_t)a[i] * b[i];
    return s;
}

#if defined(TINY_HAVE_PIE)
int32_t tiny_dot_s8_pie(const int8_t *a, const int8_t *b, int n);
#endif

#ifdef __cplusplus
}
#endif
