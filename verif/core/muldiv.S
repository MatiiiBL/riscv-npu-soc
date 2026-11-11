/*
 * M extension in the pipeline: divides consumed immediately, back-to-back
 * divides (independent and dependent), forwarded divider operands, the
 * RISC-V corner cases (divide by zero, INT_MIN / -1), divide results feeding
 * branches and stores, a divide on a squashed path, and the four multiplies.
 * Expected values come from a Python reference of the ISA semantics.
 */
#include "test.h"

        .data
        .align  4
val:    .word   5
scratch: .word  0

TEST_BEGIN
        CASE(1)                         /* result consumed by the next instruction */
        li      a1, 100
        li      a2, 7
        div     a3, a1, a2
        addi    a4, a3, 1
        CHECK(a4, 15)

        CASE(2)                         /* back-to-back independent divides */
        li      a1, -100
        li      a2, 7
        div     a3, a1, a2
        rem     a4, a1, a2
        divu    a5, a1, a2
        remu    a6, a1, a2
        CHECK(a3, -14)
        CHECK(a4, -2)
        CHECK(a5, 0x24924916)
        CHECK(a6, 2)

        CASE(3)                         /* each divide feeds the next */
        li      a1, 1000000
        li      a2, 10
        divu    a3, a1, a2
        divu    a3, a3, a2
        divu    a3, a3, a2
        CHECK(a3, 1000)

        CASE(4)                         /* operands from MEM/WB and EX/MEM */
        li      a1, 1234
        li      a2, 56
        rem     a3, a1, a2
        CHECK(a3, 2)

        CASE(5)                         /* INT_MIN / -1 */
        li      a1, 0x80000000
        li      a2, -1
        div     a3, a1, a2
        rem     a4, a1, a2
        divu    a5, a1, a2
        remu    a6, a1, a2
        CHECK(a3, 0x80000000)
        CHECK(a4, 0)
        CHECK(a5, 0)
        CHECK(a6, 0x80000000)

        CASE(6)                         /* divide by zero */
        li      a1, 7
        div     a3, a1, zero
        rem     a4, a1, zero
        divu    a5, a1, zero
        remu    a6, a1, zero
        CHECK(a3, -1)
        CHECK(a4, 7)
        CHECK(a5, 0xffffffff)
        CHECK(a6, 7)
        li      a1, -7
        div     a3, a1, zero
        rem     a4, a1, zero
        divu    a5, a1, zero
        remu    a6, a1, zero
        CHECK(a3, -1)
        CHECK(a4, -7)
        CHECK(a5, 0xffffffff)
        CHECK(a6, 0xfffffff9)

        CASE(7)                         /* signs: quotient truncates toward zero */
        li      a1, -7
        li      a2, 2
        div     a3, a1, a2
        rem     a4, a1, a2
        CHECK(a3, -3)
        CHECK(a4, -1)
        li      a1, 7
        li      a2, -2
        div     a3, a1, a2
        rem     a4, a1, a2
        CHECK(a3, -3)
        CHECK(a4, 1)
        li      a1, -7
        div     a3, a1, a2
        rem     a4, a1, a2
        divu    a5, a1, a2
        remu    a6, a1, a2
        CHECK(a3, 3)
        CHECK(a4, -1)
        CHECK(a5, 0)
        CHECK(a6, 0xfffffff9)

        CASE(8)                         /* more edges */
        li      a1, 0x80000000
        li      a2, 2
        div     a3, a1, a2
        divu    a4, a1, a2
        div     a5, a1, a1
        CHECK(a3, 0xc0000000)
        CHECK(a4, 0x40000000)
        CHECK(a5, 1)
        li      a1, -1
        li      a2, 0x10000
        remu    a3, a1, a2
        CHECK(a3, 0xffff)

        CASE(9)                         /* result into a branch and a store */
        li      a1, 81
        li      a2, 9
        li      a4, 9
        div     a3, a1, a2
        bne     a3, a4, fail
        la      a5, scratch
        div     a3, a1, a2
        sw      a3, 0(a5)
        lw      a6, 0(a5)
        CHECK(a6, 9)

        CASE(10)                        /* divide followed by a load-use pair */
        la      a5, val
        li      a1, 50
        li      a2, 5
        div     a3, a1, a2
        lw      a4, 0(a5)
        add     a6, a4, a3
        CHECK(a6, 15)

        CASE(11)                        /* rd = x0, and rd = rs1 */
        li      a1, 9
        li      a2, 3
        div     zero, a1, a2
        add     a3, zero, zero
        CHECK(a3, 0)
        div     a1, a1, a2
        div     a1, a1, a2
        CHECK(a1, 1)

        CASE(12)                        /* a divide behind a taken jump never runs */
        li      a3, 77
        j       1f
        div     a3, a1, a2
1:      CHECK(a3, 77)

        CASE(13)                        /* the four multiplies */
        li      a1, -3
        li      a2, 5
        mul     a3, a1, a2
        mulh    a4, a1, a2
        mulhsu  a5, a1, a2
        mulhu   a6, a1, a2
        CHECK(a3, 0xfffffff1)
        CHECK(a4, 0xffffffff)
        CHECK(a5, 0xffffffff)
        CHECK(a6, 4)
        li      a1, 0x80000000
        mul     a3, a1, a1
        mulh    a4, a1, a1
        mulhsu  a5, a1, a1
        mulhu   a6, a1, a1
        CHECK(a3, 0)
        CHECK(a4, 0x40000000)
        CHECK(a5, 0xc0000000)
        CHECK(a6, 0x40000000)
        li      a1, -1
        mul     a3, a1, a1
        mulh    a4, a1, a1
        mulhsu  a5, a1, a1
        mulhu   a6, a1, a1
        CHECK(a3, 1)
        CHECK(a4, 0)
        CHECK(a5, 0xffffffff)
        CHECK(a6, 0xfffffffe)
        li      a1, 0x12345678
        li      a2, 0x9abcdef0
        mul     a3, a1, a2
        mulh    a4, a1, a2
        mulhsu  a5, a1, a2
        mulhu   a6, a1, a2
        CHECK(a3, 0x242d2080)
        CHECK(a4, 0xf8cc93d6)
        CHECK(a5, 0x0b00ea4e)
        CHECK(a6, 0x0b00ea4e)

        CASE(14)                        /* multiply and divide chained */
        li      a1, 123
        li      a2, 456
        mul     a3, a1, a2
        divu    a4, a3, a2
        mul     a5, a4, a4
        CHECK(a5, 15129)

        CASE(15)                        /* division in a loop: digit sum of 4294967295 */
        li      a1, -1
        li      a2, 10
        li      a3, 0
1:      remu    a4, a1, a2
        add     a3, a3, a4
        divu    a1, a1, a2
        bnez    a1, 1b
        CHECK(a3, 57)

TEST_END
