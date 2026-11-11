/*
 * Inner dimensions beyond KMAX. npu_gemm() must split K into chunks that
 * accumulate in the array (NO_WB on all but the last, ACC_KEEP on all but
 * the first) and apply bias and requantization once at the end. Checked
 * against sw_gemm for K just above KMAX, at multiples of it and well past it,
 * with output shapes from one element to several tiles; then the same
 * accumulation driven by hand with uneven chunk sizes through
 * npu_gemm_tile().
 *
 * Each line reports the number of commands the driver actually started
 * (its count of START writes), so expected.txt also pins how many commands
 * npu_gemm() spends: one per K chunk of each output tile is the minimum.
 */
#include "npu.h"
#include "rng.h"
#include "soc.h"

#define MAX_ROWS 13
#define MAX_K    3100
#define C_BYTES  (MAX_ROWS * MAX_ROWS * 4)
#define CANARY   0x3Cu

static int8_t a_buf[MAX_ROWS * MAX_K] __attribute__((aligned(4)));
static int8_t b_buf[MAX_ROWS * MAX_K] __attribute__((aligned(4)));
static int32_t bias_buf[MAX_ROWS] __attribute__((aligned(4)));
static uint8_t c_npu[C_BYTES] __attribute__((aligned(4)));
static uint8_t c_sw[C_BYTES] __attribute__((aligned(4)));

static rng_t rng = {0x5917C0DEu};

static gemm_op_t make_op(uint32_t m, uint32_t n, uint32_t k, int int8_out) {
    uint32_t stride = (k + 3u) & ~3u;
    gemm_op_t op = {
        .a = a_buf,
        .b = b_buf,
        .c = c_npu,
        .bias = int8_out ? bias_buf : NULL,
        .m = m,
        .n = n,
        .k = k,
        .a_stride = stride,
        .b_stride = stride,
        .c_stride = int8_out ? (n + 3u) & ~3u : n * 4u,
        /* Sums of 1000-3000 random INT8 products have a standard deviation
         * of 0.2-0.3 million; 22 / 2^16 maps that onto the INT8 range, with
         * the tails clamping. */
        .quant = int8_out ? NPU_QUANT_OUT_INT8 | NPU_QUANT_RELU | NPU_QUANT_SHIFT(16) : 0u,
        .qmult = 22,
    };
    rng_fill_i8(&rng, a_buf, m * stride);
    rng_fill_i8(&rng, b_buf, n * stride);
    for (uint32_t i = 0; i < n; i++) {
        bias_buf[i] = rng_range(&rng, -100000, 100000);
    }
    return op;
}

static void reference(const gemm_op_t *op) {
    memset(c_npu, CANARY, sizeof c_npu);
    memset(c_sw, CANARY, sizeof c_sw);
    gemm_op_t sw = *op;
    sw.c = c_sw;
    sw_gemm(&sw);
    npu_counters_reset();
}

static int untouched(const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] != CANARY) {
            return 0;
        }
    }
    return 1;
}

/*
 * Checks the result of the commands issued since reference(). On arrays
 * narrower than 4, npu_gemm() must refuse INT8 output wider than one tile
 * (NPU_UNSUPPORTED) before starting any command.
 */
static int report(const char *how, const gemm_op_t *op, int rc, uint32_t arr) {
    int int8_out = (op->quant & NPU_QUANT_OUT_INT8) != 0;
    int ok;
    if (int8_out && op->n > arr && arr < 4) {
        ok = rc == NPU_UNSUPPORTED && npu_commands() == 0 && untouched(c_npu, sizeof c_npu);
    } else {
        ok = rc == NPU_OK && memcmp(c_npu, c_sw, sizeof c_npu) == 0;
    }
    printf("%s M=%u N=%u K=%u %s: %u commands %s\n", how, op->m, op->n, op->k,
           int8_out ? "int8" : "int32", npu_commands(), ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

int main(void) {
    printf("npu_ksplit: K beyond KMAX\n");
    npu_info_t info = npu_info();
    printf("NPU: ARR=%u KMAX=%u\n", info.arr, info.kmax);
    if (info.arr < 2 || info.arr > MAX_ROWS || info.kmax < 64 || 3 * info.kmax > MAX_K) {
        printf("unexpected INFO register\nFAIL\n");
        return 1;
    }

    const uint32_t K = info.kmax;
    const uint32_t A = info.arr;
    const uint32_t cases[][3] = {
        {1, 1, K + 1},  {A, A, K + 1},     {A, A, K + 4},     {5, 3, 2 * K},
        {A, A, 2 * K + 3}, {MAX_ROWS, 9, 3 * K - 5}, {2, MAX_ROWS, 3000 < 3 * K ? 3000 : 3 * K},
    };

    int failures = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        for (int int8_out = 0; int8_out < 2; int8_out++) {
            gemm_op_t op = make_op(cases[i][0], cases[i][1], cases[i][2], int8_out);
            reference(&op);
            int rc = npu_gemm(&op);
            failures += report("npu_gemm", &op, rc, A);
        }
    }

    /* The same accumulation by hand, in uneven chunks no longer than KMAX
     * and word aligned (A and B rows must start on a word boundary). */
    static const uint32_t chunks[] = {100, 4, 1020, 336, 8, 52};
    for (int int8_out = 0; int8_out < 2; int8_out++) {
        gemm_op_t op = make_op(A, A - 1, 2000, int8_out);
        reference(&op);
        int rc = NPU_OK;
        uint32_t k0 = 0;
        for (size_t c = 0; k0 < op.k; c = (c + 1) % (sizeof chunks / sizeof chunks[0])) {
            uint32_t len = chunks[c] < op.k - k0 ? chunks[c] : op.k - k0;
            len = len < (K & ~3u) ? len : (K & ~3u);
            gemm_op_t part = op;
            part.a = op.a + k0;
            part.b = op.b + k0;
            part.k = len;
            uint32_t ctrl = (k0 > 0 ? NPU_CTRL_ACC_KEEP : 0u) | (k0 + len < op.k ? NPU_CTRL_NO_WB : 0u);
            rc |= npu_gemm_tile(&part, ctrl);
            k0 += len;
        }
        failures += report("by hand ", &op, rc, A);
    }

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures;
}
