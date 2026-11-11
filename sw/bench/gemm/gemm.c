/*
 * GEMM benchmark: the same INT8 GEMMs on the CPU (sw_gemm) and on the NPU
 * (npu_gemm), timed with rdcycle around the whole call, so NPU numbers
 * include the driver's register programming and polling. Every NPU result
 * is checked against the software one byte for byte.
 *
 * Cycle counts are only meaningful on the RTL simulator: the ISS runs NPU
 * commands instantly and its cycle counter is not a timing model.
 */
#include "bench.h"
#include "npu.h"
#include "rng.h"
#include "soc.h"

typedef struct {
    const char *name;
    uint32_t m, n, k;
    uint32_t quant; /* 0: plain INT32 GEMM; else bias + requantize + ReLU */
} bench_case_t;

static const bench_case_t cases[] = {
    {"8x8x64", 8, 8, 64, 0},
    {"32x32x32", 32, 32, 32, 0},
    {"64x64x64", 64, 64, 64, 0},
    {"64x64x256", 64, 64, 256, 0},
    {"16x16x2048", 16, 16, 2048, 0},
    {"64x64x256_q", 64, 64, 256, NPU_QUANT_OUT_INT8 | NPU_QUANT_RELU | NPU_QUANT_SHIFT(12)},
};

#define A_BYTES (32 * 1024)
#define C_BYTES (64 * 64 * 4)

static int8_t a_buf[A_BYTES] __attribute__((aligned(4)));
static int8_t b_buf[A_BYTES] __attribute__((aligned(4)));
static int32_t bias_buf[64] __attribute__((aligned(4)));
static uint8_t c_sw[C_BYTES] __attribute__((aligned(4)));
static uint8_t c_npu[C_BYTES] __attribute__((aligned(4)));

int main(void) {
    npu_info_t info = npu_info();
    rng_t rng = {0xBE5C4u};
    int failures = 0;

    printf("gemm: INT8 GEMM, software (sw_gemm) vs NPU (npu_gemm)\n");
    printf("NPU: ARR=%u KMAX=%u\n\n", info.arr, info.kmax);
    printf("%12s %8s %10s %10s %8s %8s %8s %8s  %s\n", "case", "MACs", "sw cyc", "npu cyc",
           "sw MAC/c", "npu MAC/c", "busy %", "speedup", "check");

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const bench_case_t *bc = &cases[i];
        int int8_out = (bc->quant & NPU_QUANT_OUT_INT8) != 0;
        gemm_op_t op = {
            .a = a_buf,
            .b = b_buf,
            .c = c_sw,
            .bias = int8_out ? bias_buf : NULL,
            .m = bc->m,
            .n = bc->n,
            .k = bc->k,
            .a_stride = bc->k,
            .b_stride = bc->k,
            .c_stride = bc->n * (int8_out ? 1u : 4u),
            .quant = bc->quant,
            .qmult = 3,
        };
        rng_fill_i8(&rng, a_buf, bc->m * bc->k);
        rng_fill_i8(&rng, b_buf, bc->n * bc->k);
        for (uint32_t j = 0; j < bc->n; j++) {
            bias_buf[j] = rng_range(&rng, -20000, 20000);
        }
        memset(c_sw, 0, sizeof c_sw);
        memset(c_npu, 0xFF, sizeof c_npu);

        uint32_t t0 = rdcycle();
        sw_gemm(&op);
        uint32_t t1 = rdcycle();
        op.c = c_npu;
        npu_counters_reset();
        uint32_t t2 = rdcycle();
        int rc = npu_gemm(&op);
        uint32_t t3 = rdcycle();

        uint32_t sw_cycles = t1 - t0;
        uint32_t npu_cycles = t3 - t2;
        uint32_t busy = npu_busy_cycles();
        uint32_t macs = bc->m * bc->n * bc->k;
        uint32_t out_bytes = bc->m * op.c_stride;
        int match = rc == NPU_OK && memcmp(c_sw, c_npu, out_bytes) == 0;
        failures += !match;

        char sw_rate[RATIO_BUF], npu_rate[RATIO_BUF], speedup[RATIO_BUF], busy_pct[RATIO_BUF];
        fmt_ratio(sw_rate, macs, sw_cycles, 3);
        fmt_ratio(npu_rate, macs, npu_cycles, 3);
        fmt_ratio(speedup, sw_cycles, npu_cycles, 2);
        fmt_ratio(busy_pct, busy * 100u, npu_cycles ? npu_cycles : 1u, 1);
        printf("%12s %8u %10u %10u %8s %8s %8s %7sx  %s\n", bc->name, macs, sw_cycles, npu_cycles,
               sw_rate, npu_rate, busy_pct, speedup, match ? "ok" : "MISMATCH");
        printf("BENCH gemm %s m=%u n=%u k=%u macs=%u sw_cycles=%u npu_cycles=%u npu_busy_cycles=%u "
               "sw_macs_per_cycle=%s npu_macs_per_cycle=%s speedup=%s match=%d\n",
               bc->name, bc->m, bc->n, bc->k, macs, sw_cycles, npu_cycles, busy, sw_rate, npu_rate,
               speedup, match);
    }

    printf("\n%s\n", failures ? "FAIL: NPU and software results differ" : "PASS: NPU == software on every case");
    return failures;
}
