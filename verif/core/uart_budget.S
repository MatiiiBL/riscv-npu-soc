/*
 * UART stores in the pipeline situations a run can end in with one of them
 * between MEM and WB: back to back (one store in WB while the next is in
 * MEM), one and two instructions apart, after a load-use stall, ahead of a
 * divide that holds EX, and in a loop whose branch is taken. Prints
 * "abcdefghijklmnop\n" (uart_budget.out). It does not poll UART_STATUS,
 * which always reads ready on the SIM = 1 builds that run the directed
 * tests; on the serial-UART build it loses bytes, which `make uart-serial`
 * uses to check that the harness then fails the run.
 *
 * A UART byte reaches stdout when its store retires, so wherever a
 * --max-cycles budget cuts the run, stdout holds exactly the bytes of the
 * UART stores in the commit log. `make budget-sweep` runs this program
 * through difftest with every budget up to the length of the run.
 */
#include "test.h"

        .data
gee:    .word   'g'

TEST_BEGIN
        li      s0, MMIO_BASE
        li      a0, 'a'
        li      a1, 'b'
        li      a2, 'c'
        sw      a0, 0(s0)               /* back to back */
        sw      a1, 0(s0)
        sw      a2, 0(s0)
        li      a0, 'd'
        sw      a0, 0(s0)               /* one instruction apart */
        li      a0, 'e'
        sw      a0, 0(s0)
        li      a0, 'f'
        nop
        sw      a0, 0(s0)               /* two apart */
        la      t0, gee
        lw      a0, 0(t0)
        sw      a0, 0(s0)               /* the store data is the load's result */
        li      a0, 'h'
        li      t1, 1000
        li      t2, 7
        sw      a0, 0(s0)               /* moves on to WB while EX holds the divide */
        div     t3, t1, t2
        li      a0, 'i'
        li      t0, 'p' + 1
1:      sw      a0, 0(s0)               /* 'i' to 'p', behind a taken branch */
        addi    a0, a0, 1
        bne     a0, t0, 1b
        li      a0, '\n'
        sw      a0, 0(s0)
TEST_END
