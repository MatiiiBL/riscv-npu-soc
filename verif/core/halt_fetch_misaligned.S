/*
 * A jump to an address that is not 4-byte aligned halts as an illegal
 * instruction at that address (exit code 3): the fetch reads the word 0
 * (docs/spec.md 2). Real RISC-V would trap on the jump; Pulse has no traps.
 * If the fetch returned the aligned word instead, the UART and SIM_EXIT
 * stores at `target` would print and end the run with exit code 0.
 */
#include "test.h"

TEST_BEGIN
        li      s0, MMIO_BASE
        li      a1, '!'
        la      t0, target
        jalr    zero, 2(t0)
target:
        sw      a1, 0x00(s0)
        sw      zero, 0x10(s0)
1:      j       1b
