/*
 * UART output and the end of a run. Prints "OK\n" (checked against
 * uart.out), then stores to SIM_EXIT and, right behind it, to UART_TX. The
 * run ends when the SIM_EXIT store retires; the younger UART store is then
 * still in MEM and must not reach stdout.
 */
#include "test.h"

TEST_BEGIN
        li      s0, MMIO_BASE
        li      a0, 'O'
        call    putc
        li      a0, 'K'
        call    putc
        li      a0, '\n'
        call    putc
        li      a1, '!'
        sw      zero, 0x10(s0)          /* SIM_EXIT = 0 */
        sw      a1, 0x00(s0)            /* UART_TX: must not appear */
1:      j       1b

putc:
        lw      t0, 0x04(s0)
        andi    t0, t0, 1
        beqz    t0, putc
        sw      a0, 0x00(s0)
        ret
