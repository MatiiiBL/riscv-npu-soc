/*
 * Branch patterns that train (and fool) the BHT, wrong-path instructions
 * that must leave no trace, back-to-back branches, and two branches that
 * share a BTB index. All of it must also be correct with static prediction.
 */
#include "test.h"

        .data
        .align  4
scratch: .word  0

TEST_BEGIN
        la      s0, scratch

        CASE(1)                         /* counted loop: taken 99 times, then not */
        li      a1, 0
        li      a2, 100
        li      a3, 0
1:      addi    a1, a1, 1
        add     a3, a3, a1
        bne     a1, a2, 1b
        CHECK(a3, 5050)

        CASE(2)                         /* alternating taken / not taken */
        li      a1, 0
        li      a2, 64
        li      a3, 0
1:      andi    a4, a1, 1
        beqz    a4, 2f
        addi    a3, a3, 1
2:      addi    a1, a1, 1
        bne     a1, a2, 1b
        CHECK(a3, 32)

        CASE(3)                         /* taken three times out of four */
        li      a1, 0
        li      a2, 64
        li      a3, 0
1:      andi    a4, a1, 3
        xori    a4, a4, 3
        bnez    a4, 2f
        addi    a3, a3, 1
2:      addi    a1, a1, 1
        bne     a1, a2, 1b
        CHECK(a3, 16)

        CASE(4)                         /* nested loops: inner exit mispredicts each pass */
        li      a1, 0
        li      a5, 0
1:      li      a2, 0
2:      addi    a5, a5, 1
        addi    a2, a2, 1
        li      a4, 7
        blt     a2, a4, 2b
        addi    a1, a1, 1
        li      a4, 9
        blt     a1, a4, 1b
        CHECK(a5, 63)

        CASE(5)                         /* data-dependent branch vs. a branch-free count */
        li      a1, 0xace1              /* 16-bit Galois LFSR, taps 0xb400 */
        li      a2, 200
        li      a3, 0
        li      a4, 0
        li      a7, 0xb400
1:      andi    a5, a1, 1
        add     a4, a4, a5
        beqz    a5, 2f
        addi    a3, a3, 1
2:      srli    a6, a1, 1
        neg     a5, a5
        and     a5, a5, a7
        xor     a1, a6, a5
        addi    a2, a2, -1
        bnez    a2, 1b
        bne     a3, a4, fail
        CHECK(a3, 106)

        CASE(6)                         /* fall-through of a taken branch is squashed */
        li      a5, 0
        sw      zero, 0(s0)
        li      a1, 1
        bnez    a1, 1f
        addi    a5, a5, 1
        sw      a1, 0(s0)
1:      CHECK(a5, 0)
        lw      a2, 0(s0)
        CHECK(a2, 0)

        CASE(7)                         /* target path of a branch trained taken is squashed */
        li      a1, 0
        li      a2, 8
        li      a5, 0
        sw      zero, 0(s0)
1:      addi    a1, a1, 1
        bne     a1, a2, 3f
        j       4f
3:      addi    a5, a5, 1
        sw      a5, 0(s0)
        j       1b
4:      CHECK(a5, 7)
        lw      a2, 0(s0)
        CHECK(a2, 7)

        CASE(8)                         /* halting instructions on a wrong path */
        j       1f
        .word   0                       /* illegal */
        ecall
1:      beqz    zero, 2f
        ebreak
        .word   0
2:

        CASE(9)                         /* back-to-back branches with mixed outcomes */
        li      a1, 0
        li      a2, 12
        li      a3, 0
1:      andi    a4, a1, 1
        beqz    a4, 2f                  /* taken on even passes */
        bnez    a4, 3f                  /* taken on odd passes */
        j       fail
2:      addi    a3, a3, 1
3:      addi    a1, a1, 1
        blt     a1, a2, 1b
        CHECK(a3, 6)

        CASE(10)                        /* taken branch whose target is pc + 4 */
        li      a1, 0
        li      a2, 4
1:      beq     zero, zero, 2f
2:      addi    a1, a1, 1
        bne     a1, a2, 1b
        CHECK(a1, 4)

        /*
         * Two taken branches 256 bytes apart: same index in a 64-entry BTB,
         * different tags and targets, executed alternately so each evicts
         * the other every time.
         */
        CASE(11)
        li      a1, 0
        li      a2, 20
        li      a5, 0
        j       alias_a
        .balign 256
alias_a:
        addi    a1, a1, 1
        bnez    a1, alias_a_taken
        j       fail
alias_a_taken:
        addi    a5, a5, 1
        j       alias_b
        .balign 256
alias_b:
        nop
        bne     a1, a2, alias_b_taken
        j       alias_done
alias_b_taken:
        addi    a5, a5, 2
        j       alias_a
alias_done:
        CHECK(a5, 58)

TEST_END
