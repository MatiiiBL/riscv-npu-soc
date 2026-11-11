/*
 * One NPU command whose C overlaps memory the same command reads or writes.
 * The order in which the NPU reads and writes decides the result:
 *
 *   C over the bias   the bias vector is read once, before any C write, so
 *                     every row of C sees the original bias even where
 *                     row 0 has already overwritten it
 *   overlapping rows  C_STRIDE smaller than a row: elements are written in
 *                     the order of spec 5.1 (row by row, column by column),
 *                     so a later element wins
 *   C over A          A is read in full before write-back starts, so C may
 *                     replace its own operand
 *
 * Every case is a 2 x 2 x 4 INT32 GEMM with a literal expected result.
 */
#include "npu.h"
#include "soc.h"

#define CANARY 77777777

static uint32_t failures;

static void check(const char *what, int rc, const int32_t *got, const int32_t *want, uint32_t n) {
    int ok = rc == NPU_OK;
    printf("%s:", what);
    for (uint32_t i = 0; i < n; i++) {
        printf(" %d", (int)got[i]);
        ok &= got[i] == want[i];
    }
    printf(" %s\n", ok ? "ok" : "FAIL");
    failures += !ok;
}

int main(void) {
    printf("npu_overlap: C overlapping the bias, itself and A\n");
    static const int8_t a[2][4] __attribute__((aligned(4))) = {{1, 0, 0, 0}, {2, 0, 0, 0}};
    static const int8_t b_ones[2][4] __attribute__((aligned(4))) = {{1, 0, 0, 0}, {1, 0, 0, 0}};
    static const int8_t b[2][4] __attribute__((aligned(4))) = {{1, 0, 0, 0}, {10, 0, 0, 0}};

    /* A x B_ones^T = [[1, 1], [2, 2]]. C row 0 is written over the bias
     * {100, 200}; row 1 still adds the original bias, not row 0's
     * {101, 201}. */
    static int32_t c_bias[2][2] __attribute__((aligned(4))) = {{100, 200}, {CANARY, CANARY}};
    gemm_op_t op = {
        .a = &a[0][0], .b = &b_ones[0][0], .c = c_bias, .bias = &c_bias[0][0],
        .m = 2, .n = 2, .k = 4, .a_stride = 4, .b_stride = 4, .c_stride = 8,
        .quant = 0, .qmult = 1,
    };
    int rc = npu_gemm_tile(&op, 0);
    check("C over bias", rc, &c_bias[0][0], (const int32_t[]){101, 201, 102, 202}, 4);

    /* A x B^T = [[1, 10], [2, 20]] from here on. Rows 4 bytes apart:
     * C[0][1] and C[1][0] share a word, and C[1][0] is written later. */
    static int32_t c_rows[4] __attribute__((aligned(4))) = {CANARY, CANARY, CANARY, CANARY};
    op.b = &b[0][0];
    op.c = c_rows;
    op.bias = NULL;
    op.c_stride = 4;
    rc = npu_gemm_tile(&op, 0);
    check("overlapping C rows", rc, c_rows, (const int32_t[]){1, 2, 20, CANARY}, 4);

    /* C (16 bytes) over A (two 4-byte rows) and the canary words after it. */
    static union {
        int8_t a[2][4];
        int32_t c[4];
    } in_place __attribute__((aligned(4))) = {.c = {0, 0, CANARY, CANARY}};
    memcpy(in_place.a, a, sizeof a);
    op.a = &in_place.a[0][0];
    op.c = in_place.c;
    op.c_stride = 8;
    rc = npu_gemm_tile(&op, 0);
    check("C over A", rc, in_place.c, (const int32_t[]){1, 10, 2, 20}, 4);

    printf("%s\n", failures ? "FAIL" : "PASS");
    return (int)failures;
}
