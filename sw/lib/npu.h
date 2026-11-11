/*
 * Driver for the Pulse NPU (docs/spec.md section 5).
 *
 * The NPU computes one output tile of at most ARR x ARR elements per
 * command, with K up to KMAX. npu_gemm_tile() issues exactly one command;
 * npu_gemm() covers any M x N x K by tiling over M and N and splitting K into
 * KMAX-sized chunks that accumulate in the array (NO_WB, then ACC_KEEP). ARR
 * and KMAX are read from the INFO register, so the same binary runs on any
 * array size. All calls are synchronous: they return once the NPU is idle.
 *
 * Every pointer and stride handed to the NPU must be a multiple of 4; the
 * NPU rejects anything else with STATUS.ERR (and touches no memory), which
 * the functions below report as NPU_ERROR.
 */
#ifndef PULSE_NPU_H
#define PULSE_NPU_H

#include "gemm.h"

#define NPU_OK    0
#define NPU_ERROR (-1)

typedef struct {
    uint32_t arr;     /* the array is arr x arr processing elements */
    uint32_t kmax;    /* longest K a single command accepts         */
    uint32_t version;
} npu_info_t;

npu_info_t npu_info(void);

/*
 * One NPU command for op (op->m, op->n <= ARR and op->k <= KMAX). ctrl may
 * add NPU_CTRL_ACC_KEEP (accumulate onto the previous command's sums instead
 * of starting from zero) and NPU_CTRL_NO_WB (keep the sums in the array and
 * write nothing).
 */
int npu_gemm_tile(const gemm_op_t *op, uint32_t ctrl);

/* C = writeback(A * B^T + bias) for any M, N >= 0 and K >= 1. */
int npu_gemm(const gemm_op_t *op);

/* The NPU twin of sw_linear(). */
int npu_linear(const linear_t *l, const int8_t *x, uint32_t x_stride,
               void *y, uint32_t y_stride, uint32_t batch);

/*
 * Sum of the CYCLES register (START to DONE, as measured by the NPU itself)
 * over every command since the last reset. The rest of a call's time is the
 * CPU programming registers and polling. Always 0 on the ISS.
 */
uint32_t npu_busy_cycles(void);
void npu_busy_cycles_reset(void);

#endif /* PULSE_NPU_H */
