/*
 * Load-use hazards: a load followed immediately by an instruction that reads
 * its result, for every kind of consumer (ALU operands, branch, store data,
 * store address, load address, jalr, mul/div), sub-word loads whose
 * extension must be applied before forwarding, loads into x0, and a load
 * shadowed by a younger write to the same register.
 */
#include "test.h"

        .data
        .align  4
words:  .word   0x11223344, 0x80000001, 0x0000007f, 0xfffffffe
ptr0:   .word   ptr1
ptr1:   .word   ptr2
ptr2:   .word   0x0000cafe
fptab:  .word   target_a, target_b
bytes:  .byte   0x80, 0x7f, 0x01, 0xff
halves: .half   0x8001, 0x7ffe
        .align  2
scratch: .space 16

TEST_BEGIN
        la      s0, words

        CASE(1)                         /* rs1 of an R-type */
        lw      a1, 0(s0)
        add     a2, a1, zero
        CHECK(a2, 0x11223344)

        CASE(2)                         /* rs2 of an R-type */
        lw      a1, 4(s0)
        sub     a2, zero, a1
        CHECK(a2, 0x7fffffff)

        CASE(3)                         /* I-type */
        lw      a1, 8(s0)
        addi    a2, a1, 1
        CHECK(a2, 0x80)

        CASE(4)                         /* both operands from one load */
        lw      a1, 12(s0)
        add     a2, a1, a1
        CHECK(a2, -4)

        CASE(5)                         /* distance 2: no stall, MEM/WB forward */
        lw      a1, 0(s0)
        nop
        addi    a2, a1, 0
        CHECK(a2, 0x11223344)

        CASE(6)                         /* branch operands */
        li      a3, 0x7f
        lw      a1, 8(s0)
        bne     a1, a3, fail
        lw      a1, 8(s0)
        bne     a3, a1, fail
        lw      a1, 8(s0)
        beq     a1, a3, 1f
        j       fail
1:

        CASE(7)                         /* store data */
        la      a4, scratch
        lw      a1, 0(s0)
        sw      a1, 0(a4)
        lw      a2, 0(a4)
        CHECK(a2, 0x11223344)

        CASE(8)                         /* load address: pointer chasing */
        la      a4, ptr0
        lw      a1, 0(a4)
        lw      a1, 0(a1)
        lw      a1, 0(a1)
        CHECK(a1, 0xcafe)

        CASE(9)                         /* store address */
        la      a4, ptr1
        li      a3, 0x600d
        lw      a1, 0(a4)
        sw      a3, 0(a1)
        la      a4, ptr2
        lw      a2, 0(a4)
        CHECK(a2, 0x600d)

        CASE(10)                        /* jalr target */
        la      a4, fptab
        lw      a1, 0(a4)
        jalr    ra, 0(a1)
        CHECK(a5, 0xa)
        lw      a1, 4(a4)
        jalr    ra, 0(a1)
        CHECK(a5, 0xb)

        CASE(11)                        /* sub-word loads, extended before forwarding */
        la      a4, bytes
        lb      a1, 0(a4)
        addi    a2, a1, 0
        CHECK(a2, -128)
        lbu     a1, 0(a4)
        addi    a2, a1, 0
        CHECK(a2, 0x80)
        lb      a1, 3(a4)
        add     a2, a1, zero
        CHECK(a2, -1)
        lbu     a1, 1(a4)
        add     a2, zero, a1
        CHECK(a2, 0x7f)
        la      a4, halves
        lh      a1, 0(a4)
        add     a2, zero, a1
        CHECK(a2, 0xffff8001)
        lhu     a1, 0(a4)
        addi    a2, a1, 0
        CHECK(a2, 0x8001)
        lh      a1, 2(a4)
        addi    a2, a1, 0
        CHECK(a2, 0x7ffe)

        CASE(12)                        /* load into x0 */
        lw      zero, 0(s0)
        add     a2, zero, zero
        CHECK(a2, 0)

        CASE(13)                        /* a younger write shadows the load */
        lw      a1, 0(s0)
        addi    a1, zero, 5
        add     a2, a1, zero
        CHECK(a2, 5)

        CASE(14)                        /* multiplier and divider operands */
        lw      a1, 8(s0)
        mul     a2, a1, a1
        CHECK(a2, 16129)
        li      a3, 10
        lw      a1, 8(s0)
        divu    a2, a1, a3
        CHECK(a2, 12)
        lw      a1, 8(s0)
        rem     a2, a3, a1
        CHECK(a2, 10)

        CASE(15)                        /* store then load of the same word */
        la      a4, scratch
        li      a1, 0x01020304
        sw      a1, 4(a4)
        lw      a2, 4(a4)
        CHECK(a2, 0x01020304)
        li      a1, 0xaa
        sb      a1, 6(a4)
        lw      a2, 4(a4)
        CHECK(a2, 0x01aa0304)
        li      a1, 0xbeef
        sh      a1, 4(a4)
        lhu     a2, 4(a4)
        CHECK(a2, 0xbeef)
        lw      a2, 4(a4)
        CHECK(a2, 0x01aabeef)

        CASE(16)                        /* two loads, consumer of both */
        lw      a1, 0(s0)
        lw      a2, 4(s0)
        add     a3, a1, a2
        CHECK(a3, 0x91223345)

        CASE(17)                        /* two stalls in a row */
        la      a4, ptr0
        lw      a1, 0(a4)
        lw      a2, 0(a1)
        addi    a3, a2, 0
        la      a4, ptr2
        bne     a3, a4, fail

TEST_END

target_a:
        li      a5, 0xa
        ret
target_b:
        li      a5, 0xb
        ret
