/*
 * xorshift32 (Marsaglia, 2003): a tiny deterministic PRNG for test data.
 * Same seed, same sequence on every simulator, so outputs are reproducible.
 */
#ifndef PULSE_RNG_H
#define PULSE_RNG_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state; /* must be nonzero */
} rng_t;

static inline uint32_t rng_next(rng_t *r) {
    uint32_t x = r->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->state = x;
    return x;
}

/* Uniform in [lo, hi]; the modulo bias is irrelevant for test data. */
static inline int32_t rng_range(rng_t *r, int32_t lo, int32_t hi) {
    return lo + (int32_t)(rng_next(r) % (uint32_t)(hi - lo + 1));
}

static inline void rng_fill_i8(rng_t *r, int8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        buf[i] = (int8_t)rng_next(r);
    }
}

#endif /* PULSE_RNG_H */
