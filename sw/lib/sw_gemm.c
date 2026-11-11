/*
 * Pure-software GEMM with the NPU's exact semantics (docs/spec.md 5.1).
 *
 * This is the baseline the NPU speedups are measured against, so it is
 * written to be reasonably fast rather than merely obviously correct. The
 * output is computed in 2 x 2 register blocks, so every byte loaded feeds
 * two multiply-accumulates (one load per MAC instead of two for a plain dot
 * product), and the K loop is unrolled 4x. Both operands are walked along K,
 * which is contiguous for A and for B (stored N x K). Of the block shapes
 * tried with clang -O2 (1x4, 2x2, 2x4, 4x2, unrolled 1x/2x/4x) this one has
 * the shortest inner loop, about 3.4 instructions per MAC; larger blocks run
 * out of registers and spill.
 *
 * Accumulation uses uint32_t so that overflow wraps exactly like the
 * hardware's INT32 adders instead of being undefined behavior.
 */
#include "gemm.h"

#define MR 2 /* rows of A per block */
#define NR 2 /* rows of B per block */

int32_t sw_writeback(int32_t v, uint32_t quant, int16_t qmult) {
    int relu = (quant & NPU_QUANT_RELU) != 0;
    if (!(quant & NPU_QUANT_OUT_INT8)) {
        return (relu && v < 0) ? 0 : v;
    }
    /* 32 x 16 bit product: compiles to mul/mulh, no 64-bit runtime helpers. */
    int64_t p = (int64_t)v * qmult;
    uint32_t shift = quant & 0x1Fu;
    if (shift > 0) {
        p = (p + ((int64_t)1 << (shift - 1))) >> shift;
    }
    if (relu && p < 0) {
        p = 0;
    }
    return p < -128 ? -128 : p > 127 ? 127 : (int32_t)p;
}

/*
 * Computes the mr x nr output block at (m, n). Always inlined with constant
 * mr/nr, so the compiler fully unrolls the inner i/j loops and keeps the
 * accumulators and loaded operands in registers.
 */
static inline __attribute__((always_inline)) void block(const gemm_op_t *op, uint32_t m,
                                                        uint32_t n, uint32_t mr, uint32_t nr) {
    const int8_t *a = op->a + m * op->a_stride;
    const int8_t *b = op->b + n * op->b_stride;
    uint32_t acc[MR][NR] = {{0}};

#pragma clang loop unroll_count(4)
    for (uint32_t k = 0; k < op->k; k++) {
        int32_t av[MR];
        int32_t bv[NR];
        for (uint32_t i = 0; i < mr; i++) {
            av[i] = a[i * op->a_stride + k];
        }
        for (uint32_t j = 0; j < nr; j++) {
            bv[j] = b[j * op->b_stride + k];
        }
        for (uint32_t i = 0; i < mr; i++) {
            for (uint32_t j = 0; j < nr; j++) {
                acc[i][j] += (uint32_t)(av[i] * bv[j]);
            }
        }
    }

    for (uint32_t i = 0; i < mr; i++) {
        uint8_t *row = (uint8_t *)op->c + (m + i) * op->c_stride;
        for (uint32_t j = 0; j < nr; j++) {
            uint32_t v = acc[i][j] + (op->bias ? (uint32_t)op->bias[n + j] : 0u);
            int32_t out = sw_writeback((int32_t)v, op->quant, op->qmult);
            if (op->quant & NPU_QUANT_OUT_INT8) {
                ((int8_t *)row)[n + j] = (int8_t)out;
            } else {
                ((int32_t *)row)[n + j] = out;
            }
        }
    }
}

void sw_gemm(const gemm_op_t *op) {
    uint32_t m = 0;
    for (; m + MR <= op->m; m += MR) {
        uint32_t n = 0;
        for (; n + NR <= op->n; n += NR) {
            block(op, m, n, MR, NR);
        }
        for (; n < op->n; n++) {
            block(op, m, n, MR, 1);
        }
    }
    for (; m < op->m; m++) {
        uint32_t n = 0;
        for (; n + NR <= op->n; n += NR) {
            block(op, m, n, 1, NR);
        }
        for (; n < op->n; n++) {
            block(op, m, n, 1, 1);
        }
    }
}

void sw_linear(const linear_t *l, const int8_t *x, uint32_t x_stride,
               void *y, uint32_t y_stride, uint32_t batch) {
    gemm_op_t op = linear_as_gemm(l, x, x_stride, y, y_stride, batch);
    sw_gemm(&op);
}
