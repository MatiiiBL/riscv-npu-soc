/* Pulse NPU driver; see npu.h and docs/spec.md section 5. */
#include "npu.h"
#include "soc.h"

static uint32_t commands;
static uint32_t busy_cycles;

static inline uint32_t addr(const void *p) {
    return (uint32_t)(uintptr_t)p;
}

static inline uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

npu_info_t npu_info(void) {
    uint32_t info = REG32(NPU_INFO);
    npu_info_t r = {
        .arr = info & 0xFFu,
        .kmax = (info >> 8) & 0xFFFFu,
        .version = info >> 24,
    };
    return r;
}

uint32_t npu_commands(void) {
    return commands;
}

uint32_t npu_busy_cycles(void) {
    return busy_cycles;
}

void npu_counters_reset(void) {
    commands = 0;
    busy_cycles = 0;
}

/*
 * Starts the programmed command and waits for DONE. This is the only place
 * that writes START, so the command counter is exact. The fences order the
 * CPU's earlier stores to the operands before the NPU's DMA reads them, and
 * the NPU's results before the CPU's later loads. They cost nothing on
 * Pulse (FENCE is a no-op on an in-order core with one memory) but they are
 * what the RISC-V memory model requires, and the "memory" clobber stops the
 * compiler from moving operand stores or result loads across the command.
 */
static int run(uint32_t ctrl) {
    __asm__ volatile("fence" ::: "memory");
    REG32(NPU_CTRL) = NPU_CTRL_START | ctrl;
    commands++;
    uint32_t status;
    do {
        status = REG32(NPU_STATUS);
    } while (!(status & NPU_STATUS_DONE));
    __asm__ volatile("fence" ::: "memory");
    busy_cycles += REG32(NPU_CYCLES);
    return (status & NPU_STATUS_ERR) ? NPU_ERROR : NPU_OK;
}

/* Registers that stay the same for every tile of one GEMM. */
static void program_common(const gemm_op_t *op) {
    REG32(NPU_A_STRIDE) = op->a_stride;
    REG32(NPU_B_STRIDE) = op->b_stride;
    REG32(NPU_C_STRIDE) = op->c_stride;
    REG32(NPU_QUANT) = op->quant;
    REG32(NPU_QMULT) = (uint16_t)op->qmult;
}

int npu_gemm_tile(const gemm_op_t *op, uint32_t ctrl) {
    program_common(op);
    REG32(NPU_A_ADDR) = addr(op->a);
    REG32(NPU_B_ADDR) = addr(op->b);
    REG32(NPU_C_ADDR) = addr(op->c);
    REG32(NPU_BIAS_ADDR) = addr(op->bias);
    REG32(NPU_M) = op->m;
    REG32(NPU_N) = op->n;
    REG32(NPU_K) = op->k;
    return run(ctrl & (NPU_CTRL_ACC_KEEP | NPU_CTRL_NO_WB | NPU_CTRL_A_KEEP | NPU_CTRL_B_KEEP));
}

/* Issues every K chunk of the mt x nt output tile at (m0, n0). */
static int gemm_tile_k(const gemm_op_t *op, uint32_t m0, uint32_t n0, uint32_t mt, uint32_t nt,
                       uint32_t kchunk, uint32_t keep) {
    uint32_t a_row = addr(op->a) + m0 * op->a_stride;
    uint32_t b_row = addr(op->b) + n0 * op->b_stride;
    uint32_t c_elem = (op->quant & NPU_QUANT_OUT_INT8) ? 1u : 4u;
    REG32(NPU_M) = mt;
    REG32(NPU_N) = nt;
    REG32(NPU_C_ADDR) = addr(op->c) + m0 * op->c_stride + n0 * c_elem;
    REG32(NPU_BIAS_ADDR) = op->bias ? addr(op->bias + n0) : 0u;
    for (uint32_t k0 = 0; k0 < op->k; k0 += kchunk) {
        uint32_t kt = min_u32(kchunk, op->k - k0);
        REG32(NPU_A_ADDR) = a_row + k0;
        REG32(NPU_B_ADDR) = b_row + k0;
        REG32(NPU_K) = kt;
        uint32_t ctrl = keep | (k0 > 0 ? NPU_CTRL_ACC_KEEP : 0u) |
                        (k0 + kt < op->k ? NPU_CTRL_NO_WB : 0u);
        if (run(ctrl) != NPU_OK) {
            return NPU_ERROR;
        }
    }
    return NPU_OK;
}

/*
 * Each output tile is finished (all of its K chunks) before the next starts,
 * because the array holds a single tile of accumulators. Between commands
 * only the registers that change are rewritten: the operand addresses, plus
 * M/N/K at the edges.
 *
 * Every command's C_ADDR must be a multiple of 4. Row tiles start whole C
 * rows apart, which C_STRIDE keeps aligned, but a column tile of an INT8 C
 * starts n0 bytes into the row, so INT8 column tiles are ARR rounded down
 * to a multiple of 4 wide (on a 6 x 6 array, 6 x 4 tiles). That only
 * matters when there is more than one column tile: an INT8 C at most ARR
 * wide is a single column, on any array.
 *
 * Loading operands dominates an NPU command, and the NPU can skip reloading
 * A or B when a command asks for the rows it loaded last time (A_KEEP /
 * B_KEEP; it compares address, stride, row count and K, so a mismatch just
 * reloads). The inner loop therefore runs over whichever of M and N has more
 * tiles, holding the other operand fixed: for a batch of 8 images through a
 * 128-wide layer, A is loaded once instead of 16 times. The first command
 * of a call never asks to keep, since the caller may have rewritten the
 * buffers since the previous call. With K split into chunks the operand
 * addresses change every command, so nothing is kept.
 *
 * K is split only when it exceeds KMAX, into chunks of KMAX rounded down to
 * a multiple of 4 so that every chunk of an A or B row starts on a word
 * boundary, as the NPU requires. The bias is applied by the final chunk
 * only, because write-back (where the NPU adds it) happens only there.
 */
int npu_gemm(const gemm_op_t *op) {
    if (op->m == 0 || op->n == 0) {
        return NPU_OK;
    }
    if (op->k == 0) {
        return NPU_ERROR;
    }
    npu_info_t info = npu_info();
    uint32_t arr = info.arr;
    int int8_columns = (op->quant & NPU_QUANT_OUT_INT8) && op->n > arr;
    uint32_t n_step = int8_columns ? arr & ~3u : arr;
    uint32_t kchunk = op->k <= info.kmax ? op->k : info.kmax & ~3u;
    if (n_step == 0 || kchunk == 0) {
        return NPU_UNSUPPORTED;
    }
    uint32_t m_tiles = (op->m + arr - 1) / arr;
    uint32_t n_tiles = (op->n + n_step - 1) / n_step;
    int n_inner = n_tiles >= m_tiles;
    uint32_t keep = n_inner ? NPU_CTRL_A_KEEP : NPU_CTRL_B_KEEP;
    uint32_t outer_tiles = n_inner ? m_tiles : n_tiles;
    uint32_t inner_tiles = n_inner ? n_tiles : m_tiles;

    program_common(op);
    for (uint32_t outer = 0; outer < outer_tiles; outer++) {
        for (uint32_t inner = 0; inner < inner_tiles; inner++) {
            uint32_t m0 = (n_inner ? outer : inner) * arr;
            uint32_t n0 = (n_inner ? inner : outer) * n_step;
            uint32_t first = outer == 0 && inner == 0;
            if (gemm_tile_k(op, m0, n0, min_u32(arr, op->m - m0), min_u32(n_step, op->n - n0),
                            kchunk, first ? 0u : keep) != NPU_OK) {
                return NPU_ERROR;
            }
        }
    }
    return NPU_OK;
}

int npu_linear(const linear_t *l, const int8_t *x, uint32_t x_stride,
               void *y, uint32_t y_stride, uint32_t batch) {
    gemm_op_t op = linear_as_gemm(l, x, x_stride, y, y_stride, batch);
    return npu_gemm(&op);
}
