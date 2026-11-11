/*
 * Running off the end of IMEM halts as an illegal instruction (exit code 3):
 * after the last IMEM word comes a fetch from 0x0001_0000, outside IMEM,
 * which reads the word 0 (docs/spec.md 3). The test jumps to the last two
 * words and falls through. Were IMEM to wrap around instead, execution would
 * restart at address 0 and never end.
 */
#include "test.h"

TEST_BEGIN
        j       last
        .org    IMEM_SIZE - 8
last:
        nop
        nop
