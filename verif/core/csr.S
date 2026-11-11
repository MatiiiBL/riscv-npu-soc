/*
 * Zicsr counters. instret must count exactly the instructions retired before
 * the reading instruction, including older ones still in flight and never
 * squashed ones, because the ISS comparison depends on it. Also: aliases,
 * high halves, cycle monotonicity, unimplemented CSRs and ignored writes.
 */
#include "test.h"

        .data
        .align  4
word:   .word   41

TEST_BEGIN
        la      s0, word

        CASE(1)                         /* consecutive reads */
        rdinstret a1
        rdinstret a2
        sub     a3, a2, a1
        CHECK(a3, 1)

        CASE(2)                         /* five instructions in between */
        rdinstret a1
        nop
        nop
        nop
        nop
        nop
        rdinstret a2
        sub     a3, a2, a1
        CHECK(a3, 6)

        CASE(3)                         /* older loads still in MEM / WB are counted */
        rdinstret a1
        lw      a4, 0(s0)
        rdinstret a2
        lw      a4, 0(s0)
        nop
        rdinstret a3
        sub     a5, a2, a1
        CHECK(a5, 2)
        sub     a5, a3, a2
        CHECK(a5, 3)

        CASE(4)                         /* a divide, a load-use stall, a squashed path */
        rdinstret a1
        div     a4, a1, a1
        rdinstret a2
        sub     a5, a2, a1
        CHECK(a5, 2)
        rdinstret a1
        lw      a4, 0(s0)
        addi    a4, a4, 1
        rdinstret a2
        sub     a5, a2, a1
        CHECK(a5, 3)
        rdinstret a1
        beq     zero, zero, 1f
        nop
        nop
1:      rdinstret a2
        sub     a5, a2, a1
        CHECK(a5, 2)

        CASE(5)                         /* machine-mode alias and high halves */
        rdinstret a1
        csrr    a2, minstret
        sub     a3, a2, a1
        CHECK(a3, 1)
        csrr    a2, instreth
        CHECK(a2, 0)
        csrr    a2, minstreth
        CHECK(a2, 0)

        CASE(6)                         /* cycle advances; mcycle and time alias it */
        rdcycle a1
        rdcycle a2
        bgeu    a1, a2, fail
        csrr    a1, mcycle
        rdtime  a2
        bgeu    a1, a2, fail
        csrr    a1, cycleh
        CHECK(a1, 0)
        csrr    a1, mcycleh
        CHECK(a1, 0)
        csrr    a1, timeh
        CHECK(a1, 0)

        CASE(7)                         /* every other CSR reads as zero */
        li      a1, 5
        csrr    a1, mstatus
        CHECK(a1, 0)
        li      a1, 5
        csrr    a1, misa
        CHECK(a1, 0)
        li      a1, 5
        csrr    a1, mhartid
        CHECK(a1, 0)
        li      a1, 5
        csrr    a1, 0x7c0
        CHECK(a1, 0)

        CASE(8)                         /* writes are ignored */
        li      a2, 0x1234
        csrrw   a1, mscratch, a2
        CHECK(a1, 0)
        csrrs   a1, mscratch, a2
        CHECK(a1, 0)
        csrrwi  a1, mscratch, 5
        csrr    a1, mscratch
        CHECK(a1, 0)
        rdinstret a1
        csrrs   a3, instret, a2
        csrrwi  a4, instret, 31
        csrrci  a5, instret, 1
        sub     a3, a3, a1
        CHECK(a3, 1)
        sub     a4, a4, a1
        CHECK(a4, 2)
        sub     a5, a5, a1
        CHECK(a5, 3)

        CASE(9)                         /* CSR result forwarded to the next instruction */
        rdinstret a1
        addi    a2, a1, 0
        bne     a1, a2, fail

TEST_END
