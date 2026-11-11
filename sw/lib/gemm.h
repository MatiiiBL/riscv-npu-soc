/*
 * INT8 GEMM and fully connected layers: operation descriptors shared by the
 * NPU driver (npu.h) and the pure-software reference kernels declared here.
 *
 * Both implementations compute, for every m < M and n < N,
 *
 *     acc    = sum over k < K of A[m][k] * B[n][k]        (INT32, wrapping)
 *     C[m][n] = writeback(acc + bias[n])
 *
 * with the write-back stage of docs/spec.md section 5.1 (INT32 output, or
 * rounding requantization to INT8, each with optional ReLU). B is stored
 * N x K, i.e. transposed, which is the natural layout of a layer's weights
 * W[out][in]. Results are bit-identical between the two implementations.
 */
#ifndef PULSE_GEMM_H
#define PULSE_GEMM_H

#include <stdint.h>
#include "soc_map.h"

/* One GEMM. Strides are in bytes; the NPU needs every address and stride to
 * be a multiple of 4, the software kernels accept any value. */
typedef struct {
    const int8_t  *a;         /* A[0][0]: M x K, row-major                     */
    const int8_t  *b;         /* B[0][0]: N x K, row-major (B transposed)      */
    void          *c;         /* C[0][0]: M x N, int8_t if OUT_INT8, else int32_t */
    const int32_t *bias;      /* N values added before write-back, or NULL     */
    uint32_t       m, n, k;
    uint32_t       a_stride;
    uint32_t       b_stride;
    uint32_t       c_stride;
    uint32_t       quant;     /* NPU_QUANT encoding: SHIFT | OUT_INT8 | RELU   */
    int16_t        qmult;     /* requantization multiplier (OUT_INT8 only)     */
} gemm_op_t;

/* A fully connected layer y = writeback(x * W^T + bias), applied to a batch
 * of input rows. W is out x in with rows `in` bytes apart. */
typedef struct {
    const int8_t  *w;
    const int32_t *bias;      /* `out` values, or NULL */
    uint32_t       in;
    uint32_t       out;
    uint32_t       quant;     /* NPU_QUANT encoding, as in gemm_op_t */
    int16_t        qmult;
} linear_t;

/* The GEMM that applies `l` to `batch` rows of x (rows x_stride bytes apart),
 * writing rows of y y_stride bytes apart: A = x, B = W, C = y, M = batch. */
static inline gemm_op_t linear_as_gemm(const linear_t *l, const int8_t *x, uint32_t x_stride,
                                       void *y, uint32_t y_stride, uint32_t batch) {
    gemm_op_t op = {
        .a = x,
        .b = l->w,
        .c = y,
        .bias = l->bias,
        .m = batch,
        .n = l->out,
        .k = l->in,
        .a_stride = x_stride,
        .b_stride = l->in,
        .c_stride = y_stride,
        .quant = l->quant,
        .qmult = l->qmult,
    };
    return op;
}

/* The write-back stage for one accumulator value v = acc + bias. Returns
 * the INT8 value (as int32_t) when OUT_INT8 is set, else the INT32 value. */
int32_t sw_writeback(int32_t v, uint32_t quant, int16_t qmult);

/* Reference GEMM on the CPU, any M, N, K >= 0. */
void sw_gemm(const gemm_op_t *op);

/* Reference fully connected layer on the CPU. */
void sw_linear(const linear_t *l, const int8_t *x, uint32_t x_stride,
               void *y, uint32_t y_stride, uint32_t batch);

#endif /* PULSE_GEMM_H */
