/*
 * CoreMark port for the Pulse SoC.
 *
 * Ticks are CPU cycles (rdcycle). CoreMark's own main() always returns 0, so
 * the pass/fail verdict is made here, in portable_fini(), which main() calls
 * last: it checks the list, matrix and state CRCs of the run against the
 * reference values for the 2K performance run (the same values main()
 * checks, from core_main.c) and ends the simulation with exit code 0 only if
 * all three match.
 *
 * main() also prints "ERROR! Must execute for at least 10 secs" and "Errors
 * detected": the reporting rules require a 10 s run so that a coarse timer
 * cannot skew a published score. A simulation runs for milliseconds of
 * simulated time and counts cycles exactly, so that rule does not apply
 * here, and its message is expected; the CRC check above is what decides.
 */
#include "bench.h"
#include "coremark.h"

#define LIST_CRC   0xe714u
#define MATRIX_CRC 0x1fd7u
#define STATE_CRC  0x8e3au

volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

ee_u32 default_num_contexts = 1;

static CORE_TICKS start_cycles, stop_cycles;

void start_time(void) {
    start_cycles = rdcycle();
}

void stop_time(void) {
    stop_cycles = rdcycle();
}

CORE_TICKS get_time(void) {
    return stop_cycles - start_cycles;
}

secs_ret time_in_secs(CORE_TICKS ticks) {
    return ticks / CLOCK_HZ;
}

void portable_init(core_portable *p, int *argc, char *argv[]) {
    (void)argc;
    (void)argv;
    p->portable_id = 1;
}

static int check(const char *what, ee_u16 got, ee_u16 want) {
    printf("  %s crc 0x%04x, expected 0x%04x: %s\n", what, got, want, got == want ? "ok" : "MISMATCH");
    return got == want;
}

void portable_fini(core_portable *p) {
    /* main() passes &results[0].port; recover results[0] around it. */
    const core_results *res =
        (const core_results *)(const void *)((const char *)p - offsetof(core_results, port));
    p->portable_id = 0;

    printf("\nPulse port validation (2K performance run):\n");
    int ok = check("list  ", res->crclist, LIST_CRC);
    ok &= check("matrix", res->crcmatrix, MATRIX_CRC);
    ok &= check("state ", res->crcstate, STATE_CRC);

    CORE_TICKS cycles = get_time();
    char score[RATIO_BUF];
    /* CoreMark/MHz = iterations / (cycles / 10^6). */
    fmt_ratio(score, res->iterations * 1000000u, cycles, 3);
    printf("%u iterations in %u cycles: %s CoreMark/MHz\n", res->iterations, cycles, score);
    printf("CRC validation %s\n", ok ? "PASSED" : "FAILED");
    printf("BENCH coremark run iterations=%u cycles=%u coremark_per_mhz=%s crc=%s\n", res->iterations,
           cycles, score, ok ? "ok" : "fail");
    sim_exit(ok ? 0u : 1u);
}
