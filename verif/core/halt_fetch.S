/*
 * A jump outside IMEM halts as an illegal instruction (exit code 3): the
 * fetch reads the word 0 (docs/spec.md 3). The target lies IMEM_SIZE above
 * `alias`, so if IMEM wrapped around instead, the UART and SIM_EXIT stores
 * there would print and end the run with exit code 0.
 */
#include "test.h"

TEST_BEGIN
        li      s0, MMIO_BASE
        li      a1, '!'
        la      t0, alias
        li      t1, IMEM_SIZE
        add     t0, t0, t1
        jr      t0
alias:
        sw      a1, 0x00(s0)
        sw      zero, 0x10(s0)
1:      j       1b
