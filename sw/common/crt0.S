/*
 * Startup code. The loader (RTL testbench, ISS or FPGA bitstream) places
 * .text in IMEM and .data/.rodata in DMEM directly, so there is nothing to
 * copy; we only set up sp/gp, clear .bss and call main(). main's return
 * value becomes the simulation exit code.
 */
#include "soc_map.h"

    .section .text.init, "ax"
    .globl _start
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, _stack_top

    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:
    call    main

    li      t0, SIM_EXIT
    sw      a0, 0(t0)
3:  j       3b
