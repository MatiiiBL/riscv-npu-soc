/* Helpers shared by the benchmarks. */
#ifndef PULSE_BENCH_H
#define PULSE_BENCH_H

#include <stdint.h>

#define RATIO_BUF 24

/*
 * Formats num / den in decimal with `decimals` digits after the point
 * (truncated), e.g. fmt_ratio(buf, 1000, 3, 2) -> "333.33". Uses only 32-bit
 * arithmetic (there is no 64-bit division in the runtime), which needs
 * den < 2^32 / 10. Returns buf, which must hold RATIO_BUF bytes.
 */
const char *fmt_ratio(char *buf, uint32_t num, uint32_t den, unsigned decimals);

#endif /* PULSE_BENCH_H */
