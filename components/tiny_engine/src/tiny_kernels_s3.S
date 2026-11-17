/* tiny_kernels_s3.S - ESP32-S3 PIE (vector extension) int8 dot product.
 *
 * int32_t tiny_dot_s8_pie(const int8_t *a, const int8_t *b, int n)
 *   a2 = a (16-byte aligned when in RAM/PSRAM, or any alignment when
 *           reading zero-copy from flash),
 *   a3 = b (always 16-byte aligned),
 *   a4 = n (multiple of 16, >= 16)
 *
 * 16 int8 multiply-accumulates per EE.VMULAS into the 40-bit ACCX register.
 * Max |sum| for our sizes is 512*127*128 < 2^31, so ACCX_0 is the result.
 */
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

    .text
    .align  4
    .global tiny_dot_s8_pie
    .type   tiny_dot_s8_pie, @function
tiny_dot_s8_pie:
    entry       a1, 32
    movi.n      a8, 0
    wur.accx_0  a8
    wur.accx_1  a8
    srli        a4, a4, 4               /* blocks of 16                   */
    extui       a8, a2, 0, 4
    bnez        a8, .Lunaligned

    addi.n      a4, a4, -1              /* last block handled after loop  */
    ee.vld.128.ip   q0, a2, 16
    ee.vld.128.ip   q1, a3, 16
    loopnez     a4, .Laligned_end
    ee.vmulas.s8.accx.ld.ip q1, a3, 16, q0, q1  /* acc += q0*q1; q1 = next b */
    ee.vld.128.ip   q0, a2, 16                  /* q0 = next a               */
.Laligned_end:
    ee.vmulas.s8.accx q0, q1
    rur.accx_0  a2
    retw.n

.Lunaligned:
    /* Unaligned `a` (zero-copy weights in flash): prime SAR_BYTE and q2. */
    ee.ld.128.usar.ip q2, a2, 16
    loopnez     a4, .Lunaligned_end
    ee.vld.128.ip     q3, a2, 16
    ee.src.q.qup      q0, q2, q3                /* q0 = unaligned 16B; q2=q3 */
    ee.vld.128.ip     q1, a3, 16
    ee.vmulas.s8.accx q0, q1
.Lunaligned_end:
    rur.accx_0  a2
    retw.n
    .size   tiny_dot_s8_pie, . - tiny_dot_s8_pie

#endif
