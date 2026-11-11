/*
 * Operand forwarding: results reaching EX from EX/MEM (distance 1), MEM/WB
 * (distance 2) and the write-before-read register file (distance 3), into
 * either ALU operand, store data and address, branch comparisons and jalr;
 * the youngest producer winning; and writes to x0 never being forwarded.
 */
#include "test.h"

        .data
        .align  4
buf:    .space  16

TEST_BEGIN
        la      s0, buf

        /* ALU result into rs1 / rs2 at distances 1, 2 and 3. */
        CASE(1)
        addi    a1, zero, 11
        add     a2, a1, zero
        CHECK(a2, 11)

        CASE(2)
        addi    a1, zero, 12
        add     a2, zero, a1
        CHECK(a2, 12)

        CASE(3)
        addi    a1, zero, 13
        nop
        add     a2, a1, zero
        CHECK(a2, 13)

        CASE(4)
        addi    a1, zero, 14
        nop
        add     a2, zero, a1
        CHECK(a2, 14)

        CASE(5)
        addi    a1, zero, 15
        nop
        nop
        add     a2, a1, zero
        CHECK(a2, 15)

        CASE(6)
        addi    a1, zero, 16
        nop
        nop
        add     a2, zero, a1
        CHECK(a2, 16)

        /* Both operands forwarded at once, from different stages. */
        CASE(7)
        addi    a1, zero, 7             /* reaches EX via MEM/WB */
        addi    a2, zero, 9             /* reaches EX via EX/MEM */
        sub     a3, a1, a2
        CHECK(a3, -2)

        CASE(8)
        addi    a2, zero, 9
        addi    a1, zero, 7
        sub     a3, a1, a2
        CHECK(a3, -2)

        CASE(9)
        addi    a1, zero, 21
        add     a2, a1, a1              /* one producer, both operands */
        CHECK(a2, 42)

        /* With several writers in flight the youngest one wins. */
        CASE(10)
        addi    a1, zero, 1
        addi    a1, zero, 2
        addi    a2, a1, 0               /* EX/MEM (2) over MEM/WB (1) */
        CHECK(a2, 2)

        CASE(11)
        addi    a1, zero, 1
        addi    a1, zero, 2
        addi    a1, zero, 3
        nop
        addi    a2, a1, 0               /* MEM/WB (3) over the register file (2) */
        CHECK(a2, 3)

        /* A chain where every instruction consumes its predecessor. */
        CASE(12)
        addi    a1, zero, 1
        slli    a1, a1, 4               /* 16 */
        addi    a1, a1, 3               /* 19 */
        sub     a1, zero, a1            /* -19 */
        srai    a1, a1, 1               /* -10 */
        andi    a1, a1, 0xff            /* 0xf6 */
        CHECK(a1, 0xf6)

        /* x0 as a destination must never be forwarded, at any distance. */
        CASE(13)
        addi    a1, zero, 5
        addi    zero, zero, 7
        add     a2, zero, a1
        CHECK(a2, 5)

        CASE(14)
        addi    zero, zero, 7
        nop
        add     a2, zero, zero
        CHECK(a2, 0)

        CASE(15)
        lui     zero, 0x12345
        addi    zero, zero, 9
        nop
        or      a2, zero, zero
        CHECK(a2, 0)

        CASE(16)
        jal     zero, 1f                /* link written to x0 */
1:      add     a2, zero, zero
        CHECK(a2, 0)
        csrr    zero, instret
        add     a2, zero, zero
        CHECK(a2, 0)

        /* Store data and store address forwarded. */
        CASE(17)
        li      a1, 0x13579bdf          /* lui + addi: the addi result is forwarded */
        sw      a1, 0(s0)
        lw      a2, 0(s0)
        CHECK(a2, 0x13579bdf)

        CASE(18)
        addi    a3, s0, 8
        sw      a1, 0(a3)               /* address from EX/MEM */
        lw      a2, 8(s0)
        CHECK(a2, 0x13579bdf)

        CASE(19)
        addi    a1, zero, 0x5a
        addi    a3, s0, 12
        sb      a1, 1(a3)               /* address from EX/MEM, data from MEM/WB */
        lw      a2, 12(s0)
        CHECK(a2, 0x5a00)

        /* Branch comparisons on forwarded operands. */
        CASE(20)
        addi    a1, zero, 3
        addi    a2, zero, 3
        bne     a1, a2, fail

        CASE(21)
        addi    a1, zero, -1
        blt     zero, a1, fail
        bgeu    zero, a1, fail

        CASE(22)
        addi    a1, zero, 4
        addi    a2, zero, 4
        beq     a1, a2, 1f
        j       fail
1:

        /* jalr base and link register forwarded. */
        CASE(23)
        la      a1, 1f
        jalr    ra, 0(a1)
        j       fail
1:

        CASE(24)
        la      a4, 2f
        jal     ra, 2f
2:      bne     ra, a4, fail

        /* LUI, AUIPC and MUL results forwarded. */
        CASE(25)
        lui     a1, 0x12345
        addi    a2, a1, 0x678
        CHECK(a2, 0x12345678)

        CASE(26)
3:      auipc   a1, 0
        la      a2, 3b
        bne     a1, a2, fail

        CASE(27)
        addi    a1, zero, 7
        mul     a2, a1, a1
        add     a3, a2, a2
        CHECK(a3, 98)

        CASE(28)
        mul     a2, a1, a1
        nop
        add     a3, a2, a1
        CHECK(a3, 56)

TEST_END
