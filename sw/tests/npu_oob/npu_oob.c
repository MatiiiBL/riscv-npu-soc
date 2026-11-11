/*
 * NPU DMA outside DMEM (docs/spec.md 4.5: such reads return 0 and such
 * writes are dropped): operands, bias and C in IMEM, straddling the end of
 * DMEM, past it, and in the MMIO window. Every case is a 2 x 2 x 8 GEMM
 * with a literal expected result.
 *
 * A DMA that wrapped out-of-range addresses back into DMEM (by dropping
 * their upper bits) would read or overwrite DMEM instead, so the cases past
 * the end are placed exactly 256 KiB above a buffer of known content, where
 * the wrapped access would land: a wrapped read gives a different result
 * and a wrapped write changes the buffer. The first bytes of DMEM, where a
 * wrapped MMIO address would land, are checked too.
 *
 * The cases use the last bytes of DMEM, which normally hold the top of the
 * stack, so this program's Makefile moves the stack below them.
 */
#include "npu.h"
#include "soc.h"

#define DMEM_END  (DMEM_BASE + DMEM_SIZE)
#define TOP_BYTES 16 /* the end of DMEM that the cases use */
#define CANARY    77777777 /* never a result of these cases */

/* From the linker script. */
extern char _stack_top[];

static const int8_t a[2][8] __attribute__((aligned(4))) = {
    {1, 2, 3, 4, 5, 6, 7, 8},
    {-1, -2, -3, -4, -5, -6, -7, -8},
};
static const int8_t b[2][8] __attribute__((aligned(4))) = {
    {1, 1, 1, 1, 1, 1, 1, 1},
    {2, 0, 2, 0, 2, 0, 2, 0},
};
static const int32_t bias[2] __attribute__((aligned(4))) = {1000, -1000};
static int32_t c[2][2] __attribute__((aligned(4)));
/* Where a wrapped write of C row 1 in case 5 would land. */
static int32_t guard[2] __attribute__((aligned(4))) = {CANARY, CANARY};

static volatile int32_t *const top = (volatile int32_t *)(DMEM_END - TOP_BYTES);
static uint32_t failures;

static uint32_t addr(const volatile void *p) {
    return (uint32_t)(uintptr_t)p;
}

/* The base case: A x B^T + bias into c; each case changes one address. */
static gemm_op_t base_op(void) {
    gemm_op_t op = {
        .a = &a[0][0], .b = &b[0][0], .c = c, .bias = bias,
        .m = 2, .n = 2, .k = 8, .a_stride = 8, .b_stride = 8, .c_stride = 8,
        .quant = 0, .qmult = 1,
    };
    for (int i = 0; i < 4; i++) {
        c[i / 2][i % 2] = CANARY;
    }
    return op;
}

static void check(const char *what, int rc, const volatile int32_t *got, const int32_t *want,
                  uint32_t n) {
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
    printf("npu_oob: NPU DMA outside DMEM\n");
    if (addr(_stack_top) > DMEM_END - TOP_BYTES) {
        printf("the stack overlaps the end of DMEM (STACK_TOP not set)\nFAIL\n");
        return 1;
    }
    uint32_t dmem_start[4];
    memcpy(dmem_start, (const void *)DMEM_BASE, sizeof dmem_start);
    /* A x B^T = [[36, 32], [-36, -32]]. */

    /* 1. A in IMEM, which holds code: reads 0, so C = bias. */
    gemm_op_t op = base_op();
    op.a = (const int8_t *)(IMEM_BASE + 0x100);
    int rc = npu_gemm_tile(&op, 0);
    check("A in IMEM", rc, &c[0][0], (const int32_t[]){1000, -1000, 1000, -1000}, 4);

    /* 2. A past the end of DMEM, 256 KiB above a: still reads 0. */
    op = base_op();
    op.a = (const int8_t *)(uintptr_t)(addr(a) + DMEM_SIZE);
    rc = npu_gemm_tile(&op, 0);
    check("A past DMEM", rc, &c[0][0], (const int32_t[]){1000, -1000, 1000, -1000}, 4);

    /* 3. One B row straddling the end: bytes 1, 2, 3, 4, then four zeros.
     * N = 1, so column 1 of C keeps the canary. */
    top[3] = 0x04030201;
    op = base_op();
    op.b = (const int8_t *)(uintptr_t)addr(&top[3]);
    op.n = 1;
    rc = npu_gemm_tile(&op, 0);
    check("B straddles DMEM end", rc, &c[0][0], (const int32_t[]){1030, CANARY, 970, CANARY}, 4);

    /* 4. The bias straddling the end: bias[0] = 500 inside, bias[1] past it. */
    top[3] = 500;
    op = base_op();
    op.bias = (const int32_t *)(uintptr_t)addr(&top[3]);
    rc = npu_gemm_tile(&op, 0);
    check("bias straddles DMEM end", rc, &c[0][0], (const int32_t[]){536, 32, 464, -32}, 4);

    /* 5. C row 0 in the last 8 bytes of DMEM, row 1 past the end, at guard
     * + 256 KiB: row 0 is written, row 1 dropped. */
    top[2] = top[3] = CANARY;
    op = base_op();
    op.c = (void *)(uintptr_t)addr(&top[2]);
    op.c_stride = addr(guard) + DMEM_SIZE - addr(&top[2]);
    rc = npu_gemm_tile(&op, 0);
    check("C row 1 past DMEM", rc, &top[2], (const int32_t[]){1036, -968}, 2);
    check("  wrapped row 1", NPU_OK, guard, (const int32_t[]){CANARY, CANARY}, 2);

    /* 6. C on UART_TX, row 1 on SIM_EXIT: if either write got through, the
     * UART would print stray bytes or the simulation would end here. */
    op = base_op();
    op.c = (void *)UART_TX;
    op.c_stride = SIM_EXIT - UART_TX;
    rc = npu_gemm_tile(&op, 0);
    printf("C in MMIO: rc=%d, still running %s\n", rc, rc == NPU_OK ? "ok" : "FAIL");
    failures += rc != NPU_OK;

    int start_ok = memcmp(dmem_start, (const void *)DMEM_BASE, sizeof dmem_start) == 0;
    printf("first 16 bytes of DMEM unchanged %s\n", start_ok ? "ok" : "FAIL");
    failures += !start_ok;

    printf("%s\n", failures ? "FAIL" : "PASS");
    return (int)failures;
}
