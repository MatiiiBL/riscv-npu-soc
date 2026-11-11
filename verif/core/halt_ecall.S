/*
 * ecall halts the core (exit code 2). The UART and SIM_EXIT stores behind it
 * are younger and must have no effect.
 */
#include "test.h"

TEST_BEGIN
        li      s0, MMIO_BASE
        li      a1, '!'
        ecall
        sw      a1, 0x00(s0)
        sw      zero, 0x10(s0)
1:      j       1b
