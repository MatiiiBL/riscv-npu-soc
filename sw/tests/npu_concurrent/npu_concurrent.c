/*
 * CPU DMEM traffic while the NPU's DMA runs. The NPU has its own DMEM port,
 * so the CPU may keep working during a command as long as it stays off
 * every DMEM word the NPU reads or writes (docs/spec.md section 5). The
 * driver never does this (it polls STATUS until DONE), so this test starts
 * each command by hand, as in spec section 5.2, then runs a fixed CPU
 * workload of word, halfword and byte loads and stores before it polls.
 *
 * The NPU's buffers and the CPU's sit interleaved in one struct, so the
 * CPU's accesses land in the DMA blocks right next to the NPU's, in every
 * bank. Each part is a whole number of 16-byte blocks (the widest DMA
 * port), so no block holds both. Afterwards C must equal sw_gemm, A, B and
 * the bias must be unchanged, and the CPU's data must equal the same
 * workload run again with the NPU idle.
 *
 * The workload is the same whatever the NPU's timing, so the ISS (which runs
 * a command instantly) prints the same text. Where the NPU takes time
 * (CYCLES != 0, i.e. on the RTL), the test also checks that it was still
 * busy after the first slice of the workload and done by the end, i.e.
 * that the CPU traffic overlapped the whole command.
 */
#include "npu.h"
#include "rng.h"
#include "soc.h"

#define ROWS      8    /* M = N = min(ARR, ROWS) */
#define K         1024
#define GAP_WORDS 64   /* CPU words in each gap: 16 blocks of 16 bytes */
#define GAPS      5
#define PASSES    2

static struct {
    uint32_t gap0[GAP_WORDS];
    int8_t a[ROWS][K];
    uint32_t gap1[GAP_WORDS];
    int8_t b[ROWS][K];
    uint32_t gap2[GAP_WORDS];
    int32_t bias[ROWS];
    uint32_t gap3[GAP_WORDS];
    int32_t c[ROWS][ROWS];
    uint32_t gap4[GAP_WORDS];
} mem __attribute__((aligned(16)));

_Static_assert(sizeof mem.bias % 16 == 0 && sizeof mem.c % 16 == 0 && sizeof mem.gap0 % 16 == 0,
               "every part of mem must be whole 16-byte blocks");

static uint32_t ref[GAPS][GAP_WORDS];
static int32_t c_sw[ROWS][ROWS];
static rng_t rng = {0xC0FFEEu};

static uint32_t *const gaps[GAPS] = {mem.gap0, mem.gap1, mem.gap2, mem.gap3, mem.gap4};

/* One slice of the workload: every word of one gap, with loads and stores
 * of all three widths and every byte lane. */
static uint32_t mix(uint32_t *gap, uint32_t salt, uint32_t sum) {
    volatile uint32_t *w = gap;
    volatile uint16_t *h = (volatile uint16_t *)gap;
    volatile uint8_t *bytes = (volatile uint8_t *)gap;
    for (uint32_t i = 0; i < GAP_WORDS; i++) {
        uint32_t v = w[i];
        w[i] = v ^ (v >> 7) ^ salt;
        bytes[4 * i + (i & 3)] = (uint8_t)(i + salt);
        h[2 * i + ((i >> 2) & 1)] ^= (uint16_t)(v >> 16);
        sum = sum * 31u + w[i];
    }
    return sum;
}

static uint32_t hash(const void *p, uint32_t len) {
    const uint8_t *s = p;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        h = (h ^ s[i]) * 16777619u;
    }
    return h;
}

static void start(const gemm_op_t *op) {
    REG32(NPU_A_ADDR) = (uint32_t)(uintptr_t)op->a;
    REG32(NPU_B_ADDR) = (uint32_t)(uintptr_t)op->b;
    REG32(NPU_C_ADDR) = (uint32_t)(uintptr_t)op->c;
    REG32(NPU_BIAS_ADDR) = (uint32_t)(uintptr_t)op->bias;
    REG32(NPU_M) = op->m;
    REG32(NPU_N) = op->n;
    REG32(NPU_K) = op->k;
    REG32(NPU_A_STRIDE) = op->a_stride;
    REG32(NPU_B_STRIDE) = op->b_stride;
    REG32(NPU_C_STRIDE) = op->c_stride;
    REG32(NPU_QUANT) = op->quant;
    REG32(NPU_QMULT) = (uint16_t)op->qmult;
    __asm__ volatile("fence" ::: "memory");
    REG32(NPU_CTRL) = NPU_CTRL_START;
}

static int run_case(const char *name, uint32_t dim, uint32_t quant, int16_t qmult) {
    rng_fill_i8(&rng, &mem.a[0][0], sizeof mem.a);
    rng_fill_i8(&rng, &mem.b[0][0], sizeof mem.b);
    for (uint32_t i = 0; i < ROWS; i++) {
        mem.bias[i] = rng_range(&rng, -50000, 50000);
    }
    for (uint32_t g = 0; g < GAPS; g++) {
        for (uint32_t i = 0; i < GAP_WORDS; i++) {
            gaps[g][i] = ref[g][i] = rng_next(&rng);
        }
    }
    memset(mem.c, 0xA5, sizeof mem.c);
    uint32_t operands = hash(mem.a, sizeof mem.a) ^ hash(mem.b, sizeof mem.b) ^
                        hash(mem.bias, sizeof mem.bias);
    gemm_op_t op = {
        .a = &mem.a[0][0], .b = &mem.b[0][0], .c = mem.c,
        .bias = (quant & NPU_QUANT_OUT_INT8) ? mem.bias : NULL,
        .m = dim, .n = dim, .k = K, .a_stride = K, .b_stride = K, .c_stride = sizeof mem.c[0],
        .quant = quant, .qmult = qmult,
    };

    start(&op);
    uint32_t sum = 0, busy_after_first = 0;
    for (uint32_t pass = 0; pass < PASSES; pass++) {
        for (uint32_t g = 0; g < GAPS; g++) {
            sum = mix(gaps[g], pass * GAPS + g, sum);
            if (pass == 0 && g == 0) {
                busy_after_first = REG32(NPU_STATUS) & NPU_STATUS_BUSY;
            }
        }
    }
    uint32_t done_at_end = REG32(NPU_STATUS) & NPU_STATUS_DONE;
    uint32_t status;
    do {
        status = REG32(NPU_STATUS);
    } while (!(status & NPU_STATUS_DONE));
    __asm__ volatile("fence" ::: "memory");
    uint32_t cycles = REG32(NPU_CYCLES);

    uint32_t ref_sum = 0;
    for (uint32_t pass = 0; pass < PASSES; pass++) {
        for (uint32_t g = 0; g < GAPS; g++) {
            ref_sum = mix(ref[g], pass * GAPS + g, ref_sum);
        }
    }
    int cpu_ok = sum == ref_sum;
    for (uint32_t g = 0; g < GAPS; g++) {
        cpu_ok &= memcmp(gaps[g], ref[g], sizeof ref[g]) == 0;
    }
    gemm_op_t sw = op;
    sw.c = c_sw;
    memset(c_sw, 0xA5, sizeof c_sw);
    sw_gemm(&sw);
    int c_ok = !(status & NPU_STATUS_ERR) && memcmp(mem.c, c_sw, sizeof c_sw) == 0;
    int operands_ok = operands == (hash(mem.a, sizeof mem.a) ^ hash(mem.b, sizeof mem.b) ^
                                   hash(mem.bias, sizeof mem.bias));
    int overlap_ok = cycles == 0 || (busy_after_first && done_at_end);

    printf("%s %ux%ux%u: C %s, operands %s, CPU data %s\n", name, dim, dim, (uint32_t)K,
           c_ok ? "ok" : "WRONG", operands_ok ? "ok" : "CHANGED", cpu_ok ? "ok" : "WRONG");
    if (!overlap_ok) {
        printf("  the CPU work did not overlap the whole command (busy after the first slice %u, "
               "done at the end %u, %u NPU cycles)\n", busy_after_first != 0, done_at_end != 0,
               cycles);
    }
    return !(c_ok && operands_ok && cpu_ok && overlap_ok);
}

int main(void) {
    printf("npu_concurrent: CPU DMEM traffic during NPU DMA\n");
    npu_info_t info = npu_info();
    if (info.arr < 2 || info.kmax < K) {
        printf("unexpected INFO register\nFAIL\n");
        return 1;
    }
    uint32_t dim = info.arr < ROWS ? info.arr : ROWS;
    int failures = 0;
    failures += run_case("int32", dim, 0, 1);
    /* Sums of 1024 random products have a standard deviation near 175000;
     * 24 / 2^16 maps that to about 64. */
    failures += run_case("int8 bias relu", dim,
                         NPU_QUANT_OUT_INT8 | NPU_QUANT_RELU | NPU_QUANT_SHIFT(16), 24);
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures;
}
