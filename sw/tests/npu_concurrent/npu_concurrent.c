/*
 * CPU DMEM traffic while the NPU's DMA runs. The NPU has its own DMEM port,
 * so the CPU may keep working during a command as long as it stays off
 * every DMEM word the NPU reads or writes (docs/spec.md section 5). The
 * driver never does this (it polls STATUS until DONE), so this test starts
 * each command by hand, as in spec section 5.2, then runs a CPU workload of
 * word, halfword and byte loads and stores before it polls.
 *
 * The NPU's buffers and the CPU's sit interleaved in one struct, so the
 * CPU's accesses land in the DMA blocks right next to the NPU's, in every
 * bank. Each part is a whole number of 16-byte blocks (the widest DMA
 * port), so no block holds both. Afterwards C must equal sw_gemm, A, B and
 * the bias must be unchanged, and the CPU's data must equal the same
 * workload run again with the NPU idle.
 *
 * The workload must outlast the command on any core, however fast, so it is
 * sized in retired instructions rather than time: it runs slices until it
 * has retired more instructions than an upper bound on the command's length
 * in NPU cycles. A core retires at most one instruction per cycle, so the
 * NPU is done by the end whatever the core's CPI. Instruction counts are
 * the same on every core configuration and on the ISS, so the program's
 * instruction stream is too. Where the NPU takes time (CYCLES != 0, i.e. on
 * the RTL), the test checks that the NPU was busy right after START and
 * done when the workload ended: the CPU traffic overlapped the whole command.
 */
#include "npu.h"
#include "rng.h"
#include "soc.h"

#define ROWS      8    /* M = N = min(ARR, ROWS) */
#define KCAP      1024 /* K = min(KMAX, KCAP) */
#define GAP_WORDS 64   /* CPU words in each gap: 16 blocks of 16 bytes */
#define GAPS      5

static struct {
    uint32_t gap0[GAP_WORDS];
    int8_t a[ROWS][KCAP];
    uint32_t gap1[GAP_WORDS];
    int8_t b[ROWS][KCAP];
    uint32_t gap2[GAP_WORDS];
    int32_t bias[ROWS];
    uint32_t gap3[GAP_WORDS];
    int32_t c[ROWS][ROWS];
    uint32_t gap4[GAP_WORDS];
} mem __attribute__((aligned(16)));

_Static_assert(sizeof mem.a % 16 == 0 && sizeof mem.bias % 16 == 0 && sizeof mem.c % 16 == 0 &&
                   sizeof mem.gap0 % 16 == 0,
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

/*
 * Twice the longest the command can take: the NPU cycle formula of
 * docs/npu.md section 6 (LOAD + COMPUTE + WB) with the narrowest, 4-byte,
 * DMA port, where LOAD is (M + N) * ceil(K / 4) + N bias words + 1.
 */
static uint32_t npu_cycle_bound(const gemm_op_t *op) {
    uint32_t load = (op->m + op->n) * ((op->k + 3) / 4) + op->n + 1;
    uint32_t compute = op->k + op->m + op->n + 2;
    uint32_t wb = op->m * op->n + 6;
    return 2 * (load + compute + wb);
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

static int run_case(const char *name, uint32_t dim, uint32_t k, uint32_t quant, int16_t qmult) {
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
        .m = dim, .n = dim, .k = k, .a_stride = KCAP, .b_stride = KCAP,
        .c_stride = sizeof mem.c[0], .quant = quant, .qmult = qmult,
    };
    uint32_t bound = npu_cycle_bound(&op);

    start(&op);
    uint32_t busy_at_start = REG32(NPU_STATUS) & NPU_STATUS_BUSY;
    uint32_t first = rdinstret(), sum = 0, slices = 0;
    do {
        sum = mix(gaps[slices % GAPS], slices, sum);
        slices++;
    } while (rdinstret() - first < bound);
    uint32_t done_at_end = REG32(NPU_STATUS) & NPU_STATUS_DONE;
    uint32_t status;
    do {
        status = REG32(NPU_STATUS);
    } while (!(status & NPU_STATUS_DONE));
    __asm__ volatile("fence" ::: "memory");
    uint32_t cycles = REG32(NPU_CYCLES);

    uint32_t ref_sum = 0;
    for (uint32_t s = 0; s < slices; s++) {
        ref_sum = mix(ref[s % GAPS], s, ref_sum);
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
    int overlap_ok = cycles == 0 || (busy_at_start && done_at_end);

    printf("%s %ux%ux%u: C %s, operands %s, CPU data %s\n", name, dim, dim, k,
           c_ok ? "ok" : "WRONG", operands_ok ? "ok" : "CHANGED", cpu_ok ? "ok" : "WRONG");
    if (!overlap_ok) {
        printf("  the CPU work did not overlap the whole command: busy right after START %u, "
               "done after the work %u; the command took %u NPU cycles, the work at least %u "
               "instructions\n", busy_at_start != 0, done_at_end != 0, cycles, bound);
    }
    return !(c_ok && operands_ok && cpu_ok && overlap_ok);
}

int main(void) {
    printf("npu_concurrent: CPU DMEM traffic during NPU DMA\n");
    npu_info_t info = npu_info();
    if (info.arr == 0 || info.kmax == 0) {
        printf("unexpected INFO register\nFAIL\n");
        return 1;
    }
    uint32_t dim = info.arr < ROWS ? info.arr : ROWS;
    uint32_t k = info.kmax < KCAP ? info.kmax : KCAP;
    int failures = 0;
    failures += run_case("int32", dim, k, 0, 1);
    /* Sums of 1024 random products have a standard deviation near 175000;
     * 24 / 2^16 maps that to about 64. */
    failures += run_case("int8 bias relu", dim, k,
                         NPU_QUANT_OUT_INT8 | NPU_QUANT_RELU | NPU_QUANT_SHIFT(16), 24);
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures;
}
