/*
 * The eight M-extension instructions, issued through inline asm so the
 * compiler cannot fold or rewrite them, against a reference built only from
 * add, shift and compare: shift-and-add multiplication into a pair of 32-bit
 * words and restoring division. No 64-bit C arithmetic is used anywhere,
 * because on RV32IM that would itself compile to mul/mulhu. Covers every pair
 * of a set of corner-case operands, the spec's division corner cases with
 * literal expected values, random operands, and back-to-back dependent
 * M instructions (forwarding out of the multiplier and the divider).
 */
#include "rng.h"
#include "soc.h"

#define M_OP(name)                                                              \
    static inline uint32_t hw_##name(uint32_t a, uint32_t b) {                  \
        uint32_t r;                                                             \
        __asm__ volatile(#name " %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));       \
        return r;                                                               \
    }
M_OP(mul)
M_OP(mulh)
M_OP(mulhsu)
M_OP(mulhu)
M_OP(div)
M_OP(divu)
M_OP(rem)
M_OP(remu)

/* ---- Reference: no M instructions ---- */

typedef struct {
    uint32_t hi, lo;
} u64_t;

static u64_t umul(uint32_t a, uint32_t b) {
    u64_t p = {0, 0};
    for (int i = 0; i < 32; i++) {
        if ((b >> i) & 1u) {
            uint32_t add_lo = a << i;
            uint32_t add_hi = i ? a >> (32 - i) : 0;
            uint32_t lo = p.lo + add_lo;
            p.hi += add_hi + (lo < p.lo);
            p.lo = lo;
        }
    }
    return p;
}

static u64_t neg64(u64_t v) {
    u64_t r = {~v.hi, ~v.lo};
    r.lo += 1;
    r.hi += (r.lo == 0);
    return r;
}

static uint32_t mag(uint32_t v) {
    return (int32_t)v < 0 ? 0u - v : v;
}

static uint32_t ref_mul(uint32_t a, uint32_t b) {
    return umul(a, b).lo;
}

static uint32_t ref_mulhu(uint32_t a, uint32_t b) {
    return umul(a, b).hi;
}

static uint32_t ref_mulh(uint32_t a, uint32_t b) {
    u64_t p = umul(mag(a), mag(b));
    return (((int32_t)a < 0) != ((int32_t)b < 0)) ? neg64(p).hi : p.hi;
}

static uint32_t ref_mulhsu(uint32_t a, uint32_t b) {
    u64_t p = umul(mag(a), b);
    return (int32_t)a < 0 ? neg64(p).hi : p.hi;
}

/* Restoring division; b must be nonzero. */
static void udivmod(uint32_t a, uint32_t b, uint32_t *q, uint32_t *r) {
    uint32_t quo = 0, rem = 0;
    for (int i = 31; i >= 0; i--) {
        uint32_t top = rem >> 31;
        rem = (rem << 1) | ((a >> i) & 1u);
        if (top || rem >= b) {
            rem -= b;
            quo |= 1u << i;
        }
    }
    *q = quo;
    *r = rem;
}

static uint32_t ref_divu(uint32_t a, uint32_t b) {
    uint32_t q, r;
    if (b == 0) {
        return 0xFFFFFFFFu;
    }
    udivmod(a, b, &q, &r);
    return q;
}

static uint32_t ref_remu(uint32_t a, uint32_t b) {
    uint32_t q, r;
    if (b == 0) {
        return a;
    }
    udivmod(a, b, &q, &r);
    return r;
}

/* Signed: divide magnitudes, quotient negative iff signs differ, remainder
 * takes the dividend's sign. INT_MIN / -1 falls out as INT_MIN, remainder 0. */
static uint32_t ref_div(uint32_t a, uint32_t b) {
    uint32_t q, r;
    if (b == 0) {
        return 0xFFFFFFFFu;
    }
    udivmod(mag(a), mag(b), &q, &r);
    return (((int32_t)a < 0) != ((int32_t)b < 0)) ? 0u - q : q;
}

static uint32_t ref_rem(uint32_t a, uint32_t b) {
    uint32_t q, r;
    if (b == 0) {
        return a;
    }
    udivmod(mag(a), mag(b), &q, &r);
    return (int32_t)a < 0 ? 0u - r : r;
}

/* ---- Tests ---- */

typedef struct {
    const char *name;
    uint32_t (*hw)(uint32_t, uint32_t);
    uint32_t (*ref)(uint32_t, uint32_t);
} op_t;

static const op_t ops[] = {
    {"mul", hw_mul, ref_mul},       {"mulh", hw_mulh, ref_mulh},
    {"mulhsu", hw_mulhsu, ref_mulhsu}, {"mulhu", hw_mulhu, ref_mulhu},
    {"div", hw_div, ref_div},       {"divu", hw_divu, ref_divu},
    {"rem", hw_rem, ref_rem},       {"remu", hw_remu, ref_remu},
};

#define NOPS (sizeof ops / sizeof ops[0])

static const uint32_t corners[] = {
    0x00000000u, 0x00000001u, 0x00000002u, 0x00000003u, 0x00000007u,
    0x0000FFFFu, 0x00010000u, 0x12345678u, 0x7FFFFFFEu, 0x7FFFFFFFu,
    0x80000000u, 0x80000001u, 0xDEADBEEFu, 0xFFFF0000u, 0xFFFFFFFEu,
    0xFFFFFFFFu,
};

#define NCORNERS (sizeof corners / sizeof corners[0])
#define NRANDOM  500

/* Literal expectations straight from the ISA manual, independent of the
 * reference implementation above. */
typedef struct {
    int op; /* index into ops[] */
    uint32_t a, b, want;
} fixed_t;

static const fixed_t fixed[] = {
    {4, 7, 0, 0xFFFFFFFFu},                     /* div  by zero  -> -1        */
    {5, 7, 0, 0xFFFFFFFFu},                     /* divu by zero  -> 2^32 - 1  */
    {6, 7, 0, 7},                               /* rem  by zero  -> dividend  */
    {7, 7, 0, 7},                               /* remu by zero  -> dividend  */
    {4, 0x80000000u, 0xFFFFFFFFu, 0x80000000u}, /* INT_MIN / -1  -> INT_MIN   */
    {6, 0x80000000u, 0xFFFFFFFFu, 0},           /* INT_MIN % -1  -> 0         */
    {4, (uint32_t)-7, 2, (uint32_t)-3},         /* rounds toward zero         */
    {6, (uint32_t)-7, 2, (uint32_t)-1},
    {4, 7, (uint32_t)-2, (uint32_t)-3},
    {6, 7, (uint32_t)-2, 1},
    {1, 0x80000000u, 0x80000000u, 0x40000000u}, /* mulh(INT_MIN, INT_MIN)     */
    {3, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFEu}, /* mulhu(-1, -1)              */
    {2, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}, /* mulhsu(-1, 2^32 - 1)       */
    {2, 0x80000000u, 0xFFFFFFFFu, 0x80000000u}, /* mulhsu(INT_MIN, 2^32 - 1)  */
    {1, 0xFFFFFFFFu, 0xFFFFFFFFu, 0},           /* mulh(-1, -1) = hi of 1     */
    {0, 0x12345678u, 0x9ABCDEF0u, 0x242D2080u},
};

#define NFIXED (sizeof fixed / sizeof fixed[0])

/* A dependent chain issued back to back: each instruction consumes the
 * previous result in the very next slot. */
static uint32_t hw_chain(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r;
    __asm__ volatile(
        "mul    %0, %1, %2\n\t"
        "divu   %0, %0, %3\n\t"
        "mulhu  %0, %0, %1\n\t"
        "rem    %0, %0, %3\n\t"
        "mulh   %0, %0, %2\n\t"
        "addi   %0, %0, 1\n\t"
        "div    %0, %1, %0\n\t"
        "remu   %0, %0, %2"
        : "=&r"(r)
        : "r"(a), "r"(b), "r"(c));
    return r;
}

static uint32_t ref_chain(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = ref_mul(a, b);
    r = ref_divu(r, c);
    r = ref_mulhu(r, a);
    r = ref_rem(r, c);
    r = ref_mulh(r, b);
    r += 1;
    r = ref_div(a, r);
    return ref_remu(r, b);
}

int main(void) {
    int failures = 0;
    rng_t rng = {0xA5A5F00Du};
    uint32_t ra[NRANDOM], rb[NRANDOM];
    for (int i = 0; i < NRANDOM; i++) {
        ra[i] = rng_next(&rng);
        /* Small divisors too, so division produces large quotients. */
        rb[i] = (i & 1) ? rng_next(&rng) : rng_next(&rng) >> (rng_next(&rng) & 31u);
    }

    printf("muldiv: M extension vs shift-and-add / restoring-division reference\n");
    for (size_t o = 0; o < NOPS; o++) {
        uint32_t bad = 0;
        for (size_t i = 0; i < NCORNERS; i++) {
            for (size_t j = 0; j < NCORNERS; j++) {
                bad += ops[o].hw(corners[i], corners[j]) != ops[o].ref(corners[i], corners[j]);
            }
        }
        for (int i = 0; i < NRANDOM; i++) {
            bad += ops[o].hw(ra[i], rb[i]) != ops[o].ref(ra[i], rb[i]);
        }
        printf("%s: %u corner pairs + %u random: %s\n", ops[o].name,
               (uint32_t)(NCORNERS * NCORNERS), (uint32_t)NRANDOM, bad ? "FAIL" : "ok");
        failures += bad != 0;
    }

    uint32_t bad = 0;
    for (size_t i = 0; i < NFIXED; i++) {
        const fixed_t *f = &fixed[i];
        uint32_t hw = ops[f->op].hw(f->a, f->b);
        uint32_t ref = ops[f->op].ref(f->a, f->b);
        if (hw != f->want || ref != f->want) {
            printf("  %s(%08x, %08x): hw %08x ref %08x want %08x\n", ops[f->op].name, f->a, f->b,
                   hw, ref, f->want);
            bad++;
        }
    }
    printf("ISA corner cases: %u: %s\n", (uint32_t)NFIXED, bad ? "FAIL" : "ok");
    failures += bad != 0;

    bad = 0;
    for (int i = 0; i + 2 < NRANDOM; i++) {
        bad += hw_chain(ra[i], rb[i] | 1u, ra[i + 2] | 3u) != ref_chain(ra[i], rb[i] | 1u, ra[i + 2] | 3u);
    }
    printf("dependent chains: %u: %s\n", (uint32_t)(NRANDOM - 2), bad ? "FAIL" : "ok");
    failures += bad != 0;

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures;
}
