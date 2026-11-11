/*
 * NPU against the software reference (sw_gemm, whose own correctness rests
 * on the literal known answers in section 1). Every NPU result is compared
 * byte for byte with the reference over the whole output buffer, which is
 * pre-filled with a canary, so a write outside the M x N result (into row
 * padding, or into the unused bytes of a partially written word) is caught
 * as well as a wrong value.
 *
 *   1. known answers      literal results for GEMM, bias, ReLU, rounding,
 *                         clamping, 48-bit products and INT32 wraparound
 *   2. single tiles       random shapes up to ARR x ARR x KMAX, random
 *                         strides and write-back options (npu_gemm_tile)
 *   3. edge shapes        1 and ARR in M/N, 1 and KMAX in K
 *   4. NO_WB / ACC_KEEP   split accumulation, re-accumulation, and an
 *                         invalid command between the halves of a split
 *   5. errors             every invalid shape, address and stride: ERR set,
 *                         nothing written, the next command works
 *   6. tiled GEMM         random M x N x K beyond one tile (npu_gemm)
 *   7. linear layer       npu_linear vs sw_linear
 */
#include "npu.h"
#include "rng.h"
#include "soc.h"

#define MAX_ARR   16
#define MAX_MN    24
#define MAX_K     1024
#define A_BYTES   (MAX_ARR * (MAX_K + 8) * 2)
#define C_BYTES   (MAX_MN * (MAX_MN * 4 + 8))
#define CANARY    0xA5u

static int8_t a_buf[A_BYTES] __attribute__((aligned(4)));
static int8_t b_buf[A_BYTES] __attribute__((aligned(4)));
static int32_t bias_buf[MAX_MN] __attribute__((aligned(4)));
static uint8_t c_npu[C_BYTES] __attribute__((aligned(4)));
static uint8_t c_sw[C_BYTES] __attribute__((aligned(4)));
static int32_t scratch[MAX_MN * MAX_MN];

static npu_info_t info;
static rng_t rng = {0x0DDBA11u};

static uint32_t align4(uint32_t v) {
    return (v + 3u) & ~3u;
}

static uint32_t c_elem(uint32_t quant) {
    return (quant & NPU_QUANT_OUT_INT8) ? 1u : 4u;
}

/* Random operands for an m x n x k GEMM with strides padded by 0-2 words. */
static gemm_op_t random_op(uint32_t m, uint32_t n, uint32_t k, uint32_t quant, int with_bias) {
    gemm_op_t op = {
        .a = a_buf,
        .b = b_buf,
        .c = c_npu,
        .bias = with_bias ? bias_buf : NULL,
        .m = m,
        .n = n,
        .k = k,
        .a_stride = align4(k) + 4u * (uint32_t)rng_range(&rng, 0, 2),
        .b_stride = align4(k) + 4u * (uint32_t)rng_range(&rng, 0, 2),
        .c_stride = align4(n * c_elem(quant)) + 4u * (uint32_t)rng_range(&rng, 0, 2),
        .quant = quant,
        .qmult = 1,
    };
    rng_fill_i8(&rng, a_buf, m * op.a_stride);
    rng_fill_i8(&rng, b_buf, n * op.b_stride);
    for (uint32_t i = 0; i < n; i++) {
        bias_buf[i] = rng_range(&rng, -30000, 30000);
    }
    return op;
}

/*
 * A random write-back mode. Half of the INT8 cases pick QMULT from the
 * actual accumulator range (so outputs spread over -128..127 and some clamp)
 * instead of a raw random QMULT, which would almost always saturate.
 */
static void random_quant(gemm_op_t *op) {
    uint32_t relu = (rng_next(&rng) & 1u) ? NPU_QUANT_RELU : 0u;
    uint32_t kind = rng_next(&rng) % 4u;
    if (kind == 0) {
        op->quant = relu;
        return;
    }
    gemm_op_t probe = *op;
    probe.c = scratch;
    probe.c_stride = op->n * 4u;
    probe.quant = 0;
    sw_gemm(&probe);
    uint32_t max_abs = 1;
    for (uint32_t i = 0; i < op->m * op->n; i++) {
        uint32_t v = scratch[i] < 0 ? 0u - (uint32_t)scratch[i] : (uint32_t)scratch[i];
        max_abs = v > max_abs ? v : max_abs;
    }
    uint32_t shift = (uint32_t)rng_range(&rng, 0, 31);
    int32_t qmult = (int16_t)rng_next(&rng);
    if (kind >= 2 && shift <= 24) {
        uint32_t q = (200u << shift) / max_abs;
        qmult = (int32_t)(q > 32767u ? 32767u : (q == 0 ? 1u : q));
        if (kind == 3) {
            qmult = -qmult;
        }
    }
    op->quant = NPU_QUANT_OUT_INT8 | relu | NPU_QUANT_SHIFT(shift);
    op->qmult = (int16_t)qmult;
    op->c_stride = align4(op->n) + 4u * (uint32_t)rng_range(&rng, 0, 2);
}

/* The part of the output buffers an op can legally touch, plus a guard
 * band behind it; checking only this keeps the test fast. */
static uint32_t c_span(const gemm_op_t *op) {
    uint32_t span = op->m * op->c_stride + 16u;
    return span < C_BYTES ? span : C_BYTES;
}

static void fill_canary(uint32_t span) {
    memset(c_npu, CANARY, span);
    memset(c_sw, CANARY, span);
}

static int report_mismatch(const char *what, const gemm_op_t *op) {
    uint32_t span = c_span(op);
    if (memcmp(c_npu, c_sw, span) == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < span; i++) {
        if (c_npu[i] != c_sw[i]) {
            printf("  %s: m=%u n=%u k=%u quant=%x qmult=%d bias=%u: byte %u npu %02x sw %02x\n",
                   what, op->m, op->n, op->k, op->quant, op->qmult, op->bias != NULL, i, c_npu[i],
                   c_sw[i]);
            return 1;
        }
    }
    return 0;
}

/* Runs op on the NPU (one command, or tiled) and in software; 0 if equal. */
static int compare(const char *what, const gemm_op_t *op, int tiled) {
    fill_canary(c_span(op));
    gemm_op_t sw = *op;
    sw.c = c_sw;
    sw_gemm(&sw);
    gemm_op_t hw = *op;
    hw.c = c_npu;
    int rc = tiled ? npu_gemm(&hw) : npu_gemm_tile(&hw, 0);
    if (rc != NPU_OK) {
        printf("  %s: m=%u n=%u k=%u: NPU reported ERR\n", what, op->m, op->n, op->k);
        return 1;
    }
    return report_mismatch(what, op);
}

static int section(const char *name, uint32_t cases, uint32_t failed) {
    printf("%s: %u of %u passed\n", name, cases - failed, cases);
    return failed != 0;
}

/* ---- 1. Known answers ---- */

typedef struct {
    int8_t a, b;
    int32_t bias;
    uint32_t quant;
    int16_t qmult;
    int32_t want;
} scalar_case_t;

/* 1 x 1 x 1 products: acc = a * b, then bias and write-back. */
static const scalar_case_t scalar_cases[] = {
    {2, 3, 0, NPU_QUANT_OUT_INT8 | NPU_QUANT_SHIFT(2), 1, 2},    /* 6/4 = 1.5 rounds up     */
    {-2, 3, 0, NPU_QUANT_OUT_INT8 | NPU_QUANT_SHIFT(2), 1, -1},  /* -1.5 rounds up to -1    */
    {-17, 3, 0, NPU_QUANT_OUT_INT8 | NPU_QUANT_SHIFT(2), 1, -13}, /* -51/4 = -12.75         */
    {100, 100, 0, NPU_QUANT_OUT_INT8 | NPU_QUANT_SHIFT(4), 3, 127},  /* 1875 clamps         */
    {-100, 100, 0, NPU_QUANT_OUT_INT8 | NPU_QUANT_SHIFT(4), 3, -128},
    {-100, 100, 0, NPU_QUANT_OUT_INT8 | NPU_QUANT_RELU | NPU_QUANT_SHIFT(4), 3, 0},
    {5, 7, 0, NPU_QUANT_OUT_INT8, -3, -105},                     /* SHIFT 0: plain product  */
    {1, 1, 2147483646, NPU_QUANT_OUT_INT8 | NPU_QUANT_SHIFT(24), 1, 127},   /* 128.0 clamps */
    {-1, 1, -2147483646, NPU_QUANT_OUT_INT8 | NPU_QUANT_SHIFT(24), 1, -128}, /* -128.0      */
    /* Products wider than 32 bits: (2^31 - 1) * 100 / 2^31 = 99.99999995. */
    {1, 1, 2147483646, NPU_QUANT_OUT_INT8 | NPU_QUANT_SHIFT(31), 100, 100},
    {-1, 1, -2147483647, NPU_QUANT_OUT_INT8 | NPU_QUANT_SHIFT(31), 100, -100}, /* INT32_MIN */
    {1, 1, 2147483646, NPU_QUANT_OUT_INT8 | NPU_QUANT_SHIFT(31), -32768, -128},
    {127, 127, 2147483647, 0, 1, -2147467520},                   /* INT32 wraps             */
    {127, 127, 2147483647, NPU_QUANT_RELU, 1, 0},                /* wrapped negative, ReLU  */
    {-128, -128, -16384, 0, 1, 0},
    {-128, 127, 0, NPU_QUANT_RELU, 1, 0},
};

static int known_answers(void) {
    uint32_t failed = 0, cases = 0;

    /* A 2 x 2 x 4 GEMM worked by hand. */
    static const int8_t a[2][4] __attribute__((aligned(4))) = {{1, 2, 3, 4}, {-1, -2, -3, -4}};
    static const int8_t b[2][4] __attribute__((aligned(4))) = {{1, 1, 1, 1}, {1, -1, 1, -1}};
    static const int32_t bias[2] __attribute__((aligned(4))) = {100, -100};
    static const int32_t want[3][4] = {
        {10, -2, -10, 2},    /* no bias            */
        {110, -102, 90, -98}, /* bias               */
        {110, 0, 90, 0},     /* bias + ReLU        */
    };
    for (int v = 0; v < 3; v++) {
        int32_t c_hw[4] __attribute__((aligned(4)));
        int32_t c_ref[4];
        gemm_op_t op = {
            .a = &a[0][0], .b = &b[0][0], .c = c_hw, .bias = v ? bias : NULL,
            .m = 2, .n = 2, .k = 4, .a_stride = 4, .b_stride = 4, .c_stride = 8,
            .quant = v == 2 ? NPU_QUANT_RELU : 0u, .qmult = 1,
        };
        int rc = npu_gemm_tile(&op, 0);
        op.c = c_ref;
        sw_gemm(&op);
        cases++;
        if (rc != NPU_OK || memcmp(c_hw, want[v], sizeof c_hw) || memcmp(c_ref, want[v], sizeof c_ref)) {
            printf("  2x2x4 variant %d: npu %d %d %d %d\n", v, c_hw[0], c_hw[1], c_hw[2], c_hw[3]);
            failed++;
        }
    }

    for (size_t i = 0; i < sizeof scalar_cases / sizeof scalar_cases[0]; i++) {
        const scalar_case_t *s = &scalar_cases[i];
        int8_t av[4] __attribute__((aligned(4))) = {s->a, 0, 0, 0};
        int8_t bv[4] __attribute__((aligned(4))) = {s->b, 0, 0, 0};
        int32_t bias_v[1] __attribute__((aligned(4))) = {s->bias};
        int32_t out_hw[1] __attribute__((aligned(4))) = {0x5A5A5A5A};
        int32_t out_ref[1] = {0x5A5A5A5A};
        gemm_op_t op = {
            .a = av, .b = bv, .c = out_hw, .bias = s->bias ? bias_v : NULL,
            .m = 1, .n = 1, .k = 1, .a_stride = 4, .b_stride = 4, .c_stride = 4,
            .quant = s->quant, .qmult = s->qmult,
        };
        int rc = npu_gemm_tile(&op, 0);
        op.c = out_ref;
        sw_gemm(&op);
        int32_t hw = s->quant & NPU_QUANT_OUT_INT8 ? (int8_t)out_hw[0] : out_hw[0];
        int32_t ref = s->quant & NPU_QUANT_OUT_INT8 ? (int8_t)out_ref[0] : out_ref[0];
        /* INT8 results write one byte: the other three must keep the canary. */
        int canary_ok = !(s->quant & NPU_QUANT_OUT_INT8) ||
                        (out_hw[0] & ~0xFF) == (0x5A5A5A5A & ~0xFF);
        cases++;
        if (rc != NPU_OK || hw != s->want || ref != s->want || !canary_ok) {
            printf("  scalar case %u: npu %d sw %d want %d (word %08x)\n", (uint32_t)i, hw, ref,
                   s->want, (uint32_t)out_hw[0]);
            failed++;
        }
    }
    return section("known answers", cases, failed);
}

/* ---- 2/3. Single tiles ---- */

static int single_tiles(void) {
    uint32_t failed = 0;
    const uint32_t cases = 200;
    for (uint32_t i = 0; i < cases; i++) {
        uint32_t m = (uint32_t)rng_range(&rng, 1, (int32_t)info.arr);
        uint32_t n = (uint32_t)rng_range(&rng, 1, (int32_t)info.arr);
        uint32_t k = (i % 20 == 19) ? (uint32_t)rng_range(&rng, 65, (int32_t)info.kmax)
                                   : (uint32_t)rng_range(&rng, 1, 64);
        gemm_op_t op = random_op(m, n, k, 0, (int)(rng_next(&rng) & 1u));
        random_quant(&op);
        failed += compare("single tile", &op, 0);
    }
    return section("single tiles", cases, failed);
}

static int edge_shapes(void) {
    const uint32_t A = info.arr, K = info.kmax;
    const uint32_t shapes[][3] = {
        {1, 1, 1}, {A, A, 1}, {1, 1, K}, {A, A, K}, {A, 1, 3}, {1, A, 5}, {A, A, 4},
        {A - 1, A / 2 + 1, K - 1},
    };
    uint32_t failed = 0, cases = 0;
    for (size_t s = 0; s < sizeof shapes / sizeof shapes[0]; s++) {
        for (int mode = 0; mode < 2; mode++) {
            gemm_op_t op = random_op(shapes[s][0], shapes[s][1], shapes[s][2], 0, mode);
            if (mode) {
                random_quant(&op);
            }
            cases++;
            failed += compare("edge shape", &op, 0);
        }
    }
    return section("edge shapes", cases, failed);
}

/* ---- 4. NO_WB / ACC_KEEP ---- */

/* The tile op restricted to K columns [k0, k1). */
static gemm_op_t k_slice(const gemm_op_t *op, uint32_t k0, uint32_t k1) {
    gemm_op_t s = *op;
    s.a = op->a + k0;
    s.b = op->b + k0;
    s.k = k1 - k0;
    return s;
}

static int all_canary(const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] != CANARY) {
            return 0;
        }
    }
    return 1;
}

static int accumulate_flags(void) {
    uint32_t failed = 0, cases = 0;
    const uint32_t A = info.arr;

    /* NO_WB writes nothing. */
    gemm_op_t op = random_op(A, A, 40, NPU_QUANT_OUT_INT8 | NPU_QUANT_SHIFT(9), 1);
    op.qmult = 77;
    fill_canary(C_BYTES);
    cases++;
    if (npu_gemm_tile(&op, NPU_CTRL_NO_WB) != NPU_OK || !all_canary(c_npu, C_BYTES)) {
        printf("  NO_WB wrote to C\n");
        failed++;
    }

    /* Two- and three-way K splits must equal the single command (the bias is
     * applied once, by the final write-back). Word-aligned split points keep
     * A and B legal. */
    gemm_op_t split2[2] = {k_slice(&op, 0, 16), k_slice(&op, 16, 40)};
    gemm_op_t split3[3] = {k_slice(&op, 0, 8), k_slice(&op, 8, 28), k_slice(&op, 28, 40)};
    const uint32_t ctrl2[2] = {NPU_CTRL_NO_WB, NPU_CTRL_ACC_KEEP};
    const uint32_t ctrl3[3] = {NPU_CTRL_NO_WB, NPU_CTRL_ACC_KEEP | NPU_CTRL_NO_WB, NPU_CTRL_ACC_KEEP};
    for (int ways = 2; ways <= 3; ways++) {
        fill_canary(c_span(&op));
        gemm_op_t ref = op;
        ref.c = c_sw;
        sw_gemm(&ref);
        int rc = NPU_OK;
        for (int i = 0; i < ways; i++) {
            rc |= npu_gemm_tile(ways == 2 ? &split2[i] : &split3[i], ways == 2 ? ctrl2[i] : ctrl3[i]);
        }
        cases++;
        failed += rc != NPU_OK || report_mismatch(ways == 2 ? "2-way K split" : "3-way K split", &op);
    }

    /* ACC_KEEP after a normal command continues from its sums: running the
     * same command twice equals one command over [A A] x [B B]. */
    gemm_op_t base = random_op(A, A, 24, 0, 1);
    static int8_t a2[MAX_ARR * 48] __attribute__((aligned(4)));
    static int8_t b2[MAX_ARR * 48] __attribute__((aligned(4)));
    for (uint32_t r = 0; r < A; r++) {
        memcpy(&a2[r * 48], base.a + r * base.a_stride, 24);
        memcpy(&a2[r * 48 + 24], base.a + r * base.a_stride, 24);
        memcpy(&b2[r * 48], base.b + r * base.b_stride, 24);
        memcpy(&b2[r * 48 + 24], base.b + r * base.b_stride, 24);
    }
    fill_canary(c_span(&base));
    gemm_op_t twice = base;
    twice.a = a2;
    twice.b = b2;
    twice.k = 48;
    twice.a_stride = twice.b_stride = 48;
    twice.c = c_sw;
    sw_gemm(&twice);
    int rc = npu_gemm_tile(&base, 0);
    memset(c_npu, CANARY, c_span(&base));
    rc |= npu_gemm_tile(&base, NPU_CTRL_ACC_KEEP);
    cases++;
    failed += rc != NPU_OK || report_mismatch("ACC_KEEP after write-back", &base);

    /* A rejected command does not touch the accumulators. */
    fill_canary(c_span(&op));
    gemm_op_t ref = op;
    ref.c = c_sw;
    sw_gemm(&ref);
    gemm_op_t bad = split2[0];
    bad.m = 0;
    rc = npu_gemm_tile(&split2[0], NPU_CTRL_NO_WB);
    int bad_rc = npu_gemm_tile(&bad, NPU_CTRL_ACC_KEEP);
    rc |= npu_gemm_tile(&split2[1], NPU_CTRL_ACC_KEEP);
    cases++;
    failed += rc != NPU_OK || bad_rc != NPU_ERROR || report_mismatch("ERR inside a K split", &op);

    return section("NO_WB / ACC_KEEP", cases, failed);
}

/* ---- 5. Errors ---- */

static int errors(void) {
    gemm_op_t good = random_op(3, 5, 12, 0, 1);
    uint32_t failed = 0, cases = 0;
    for (int e = 0; e < 13; e++) {
        gemm_op_t op = good;
        switch (e) {
        case 0: op.m = 0; break;
        case 1: op.n = 0; break;
        case 2: op.k = 0; break;
        case 3: op.m = info.arr + 1; break;
        case 4: op.n = info.arr + 1; break;
        case 5: op.k = info.kmax + 1; break;
        case 6: op.a = good.a + 1; break;
        case 7: op.b = good.b + 2; break;
        case 8: op.c = (uint8_t *)good.c + 3; break;
        case 9: op.bias = (const int32_t *)((const uint8_t *)good.bias + 2); break;
        case 10: op.a_stride += 1; break;
        case 11: op.b_stride += 2; break;
        default: op.c_stride += 1; break;
        }
        fill_canary(C_BYTES);
        int rc = npu_gemm_tile(&op, 0);
        uint32_t status = REG32(NPU_STATUS);
        int quiet = all_canary(c_npu, C_BYTES);
        /* The NPU must recover: the next valid command succeeds. */
        int next = compare("command after ERR", &good, 0);
        cases++;
        if (rc != NPU_ERROR || !(status & NPU_STATUS_ERR) || !quiet || next) {
            printf("  error case %d: rc=%d status=%x wrote=%d\n", e, rc, status, !quiet);
            failed++;
        }
    }
    return section("errors", cases, failed);
}

/* ---- 6/7. Tiled GEMM and linear layer ---- */

static int tiled(void) {
    uint32_t failed = 0;
    const uint32_t cases = 40;
    for (uint32_t i = 0; i < cases; i++) {
        uint32_t m = (uint32_t)rng_range(&rng, 1, MAX_MN);
        uint32_t n = (uint32_t)rng_range(&rng, 1, MAX_MN);
        uint32_t k = (uint32_t)rng_range(&rng, 1, 160);
        gemm_op_t op = random_op(m, n, k, 0, (int)(rng_next(&rng) & 1u));
        random_quant(&op);
        failed += compare("tiled gemm", &op, 1);
    }
    return section("tiled gemm", cases, failed);
}

static int linear(void) {
    gemm_op_t data = random_op(MAX_MN, 13, 100, 0, 1);
    linear_t l = {
        .w = data.b,
        .bias = bias_buf,
        .in = 100,
        .out = 13,
        .quant = NPU_QUANT_OUT_INT8 | NPU_QUANT_RELU | NPU_QUANT_SHIFT(12),
        .qmult = 41,
    };
    /* linear_t has no B stride: W rows are `in` bytes apart. */
    rng_fill_i8(&rng, b_buf, 13 * 100);
    fill_canary(C_BYTES);
    int rc = npu_linear(&l, a_buf, data.a_stride, c_npu, 16, MAX_MN);
    sw_linear(&l, a_buf, data.a_stride, c_sw, 16, MAX_MN);
    uint32_t failed = rc != NPU_OK || memcmp(c_npu, c_sw, C_BYTES) != 0;
    return section("linear layer", 1, failed);
}

int main(void) {
    printf("npu_gemm: NPU vs software reference\n");
    info = npu_info();
    printf("NPU: ARR=%u KMAX=%u version=%u\n", info.arr, info.kmax, info.version);
    if (info.version != 1 || info.arr < 2 || info.arr > MAX_ARR || info.kmax < 64 ||
        info.kmax > MAX_K) {
        printf("unexpected INFO register\nFAIL\n");
        return 1;
    }

    int failures = 0;
    failures += known_answers();
    failures += single_tiles();
    failures += edge_shapes();
    failures += accumulate_flags();
    failures += errors();
    failures += tiled();
    failures += linear();
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures;
}
