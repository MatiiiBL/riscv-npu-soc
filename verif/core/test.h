/*
 * Directed core tests: shared macros.
 *
 * Tests run bare-metal like the riscv-tests in verif/riscv-tests: gp holds
 * the number of the case being checked, and the result goes to SIM_EXIT,
 * 0 for a pass or (case << 1) | 1 for a failure, so the failing case can be
 * read off the exit value. t6 is the scratch register of CHECK; tests must
 * not keep values in it.
 */
#ifndef PULSE_CORE_TEST_H
#define PULSE_CORE_TEST_H

#include "soc_map.h"

#define TEST_BEGIN                      \
        .section .text.init, "ax";      \
        .align  2;                      \
        .globl  _start;                 \
_start:                                 \
        la      sp, _stack_top;         \
        li      gp, 0

#define CASE(n)                         \
        li      gp, n

/* Fails the test unless reg == val. */
#define CHECK(reg, val)                 \
        li      t6, val;                \
        bne     reg, t6, fail

#define TEST_END                        \
        li      t0, SIM_EXIT;           \
        sw      zero, 0(t0);            \
1:      j       1b;                     \
fail:                                   \
        slli    a0, gp, 1;              \
        ori     a0, a0, 1;              \
        li      t0, SIM_EXIT;           \
        sw      a0, 0(t0);              \
2:      j       2b

#endif /* PULSE_CORE_TEST_H */
