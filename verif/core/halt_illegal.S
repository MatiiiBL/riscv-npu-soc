/*
 * An illegal instruction halts the core (exit code 3), but only when it is
 * on the executed path: the all-zero word behind a taken jump is squashed.
 * The halting word is `slli x1, x1, 32`, reserved in RV32 (shamt[5] = 1).
 * The UART and SIM_EXIT stores behind it must have no effect.
 */
#include "test.h"

TEST_BEGIN
        li      s0, MMIO_BASE
        li      a1, '!'
        j       1f
        .word   0x00000000
1:      .word   0x02009093
        sw      a1, 0x00(s0)
        sw      zero, 0x10(s0)
2:      j       2b
