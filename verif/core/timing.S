/*
 * Cycle-exact checks of the pipeline costs in docs/spec.md 6 and
 * docs/core.md. Two rdcycle instructions read the counter in EX, so their
 * difference is 1 + (instructions between them) + (bubbles between them).
 * Built once per predictor mode (-DBP_MODE=0 / 1) since branch costs differ.
 */
#include "test.h"

#if BP_MODE == 1
#define TAKEN_TRAINED   2       /* BTB hit, predicted taken: no bubble */
#define FLIP            4       /* trained taken, then falls through: 2 bubbles */
#define TAKEN_AGAIN     2       /* new entries start weakly taken */
#define HYSTERESIS      2       /* one not-taken does not flip a saturated counter */
#else
#define TAKEN_TRAINED   4       /* static not-taken: every taken branch costs 2 */
#define FLIP            2
#define TAKEN_AGAIN     4
#define HYSTERESIS      4
#endif
#define TAKEN_COLD      4       /* first execution misses the BTB in either mode */
#define DIV_CYCLES      34      /* cycles a divide spends in EX */
#define LOOP            4

        .data
        .align  4
word:   .word   7

TEST_BEGIN
        la      s0, word
        li      a6, 100
        li      a7, 7

        CASE(1)                         /* back to back */
        rdcycle a1
        rdcycle a2
        sub     a3, a2, a1
        CHECK(a3, 1)

        CASE(2)                         /* independent instructions issue every cycle */
        rdcycle a1
        nop
        nop
        nop
        nop
        rdcycle a2
        sub     a3, a2, a1
        CHECK(a3, 5)

        CASE(3)                         /* load-use costs exactly one bubble */
        rdcycle a1
        lw      a4, 0(s0)
        addi    a5, a4, 1
        rdcycle a2
        sub     a3, a2, a1
        CHECK(a3, 4)
        rdcycle a1
        lw      a4, 0(s0)
        sw      a4, 4(s0)               /* store data */
        rdcycle a2
        sub     a3, a2, a1
        CHECK(a3, 4)
        rdcycle a1
        lw      a4, 0(s0)
        bne     a4, a7, fail            /* branch, not taken */
        rdcycle a2
        sub     a3, a2, a1
        CHECK(a3, 4)

        CASE(4)                         /* ...and nothing without a dependency */
        rdcycle a1
        lw      a4, 0(s0)
        addi    a5, a6, 1
        rdcycle a2
        sub     a3, a2, a1
        CHECK(a3, 3)
        rdcycle a1
        lw      a4, 0(s0)
        nop
        addi    a5, a4, 1
        rdcycle a2
        sub     a3, a2, a1
        CHECK(a3, 4)

        CASE(5)                         /* multiply: the product is one cycle late */
        rdcycle a1
        mul     a4, a6, a7
        add     a5, a4, a4
        rdcycle a2
        sub     a3, a2, a1
        CHECK(a3, 4)

        CASE(6)                         /* divide holds EX; back-to-back divides */
        rdcycle a1
        div     a4, a6, a7
        rdcycle a2
        sub     a3, a2, a1
        CHECK(a3, 1 + DIV_CYCLES)
        rdcycle a1
        div     a4, a6, a7
        rem     a5, a6, a7
        rdcycle a2
        sub     a3, a2, a1
        CHECK(a3, 1 + 2 * DIV_CYCLES)
        rdcycle a1
        div     a4, a6, a7
        add     a5, a4, a4
        rdcycle a2
        sub     a3, a2, a1
        CHECK(a3, 2 + DIV_CYCLES)

        CASE(7)                         /* not-taken branch: free */
        rdcycle a1
        bne     zero, zero, fail
        rdcycle a2
        sub     a3, a2, a1
        CHECK(a3, 2)

        CASE(8)                         /* taken branch seen for the first time */
        rdcycle a1
        beq     zero, zero, 1f
        nop
1:      rdcycle a2
        sub     a3, a2, a1
        CHECK(a3, TAKEN_COLD)

        CASE(9)                         /* taken branch after training */
        li      s1, LOOP
1:      rdcycle a1
        beq     zero, zero, 2f
        nop
2:      rdcycle a2
        addi    s1, s1, -1
        bnez    s1, 1b
        sub     a3, a2, a1
        CHECK(a3, TAKEN_TRAINED)

        CASE(10)                        /* jal after training */
        li      s1, LOOP
1:      rdcycle a1
        j       2f
        nop
2:      rdcycle a2
        addi    s1, s1, -1
        bnez    s1, 1b
        sub     a3, a2, a1
        CHECK(a3, TAKEN_TRAINED)

        CASE(11)                        /* jalr to a fixed target after training */
        la      s2, 2f
        li      s1, LOOP
1:      rdcycle a1
        jr      s2
        nop
2:      rdcycle a2
        addi    s1, s1, -1
        bnez    s1, 1b
        sub     a3, a2, a1
        CHECK(a3, TAKEN_TRAINED)

        CASE(12)                        /* taken LOOP-1 times, then not taken */
        li      s1, LOOP
1:      addi    s1, s1, -1
        rdcycle a1
        bnez    s1, 2f
        rdcycle a2
        j       3f
2:      rdcycle a2
        j       1b
3:      sub     a3, a2, a1
        CHECK(a3, FLIP)

        CASE(13)                        /* second execution of a taken branch */
        li      s1, 2
1:      rdcycle a1
        beq     zero, zero, 2f
        nop
2:      rdcycle a2
        addi    s1, s1, -1
        bnez    s1, 1b
        sub     a3, a2, a1
        CHECK(a3, TAKEN_AGAIN)

        CASE(14)                        /* outcomes T T T N T: the last T is predicted */
        li      s1, 5
1:      addi    s1, s1, -1
        xori    t0, s1, 1               /* zero (not taken) when s1 == 1 */
        rdcycle a1
        bnez    t0, 2f
        rdcycle a2
        j       3f
2:      rdcycle a2
3:      bnez    s1, 1b
        sub     a3, a2, a1
        CHECK(a3, HYSTERESIS)

        /*
         * Two taken branches at the same offset in different 256-byte blocks
         * share a BTB index, so with alternating execution each one evicts
         * the other and both stay at the cold cost even after training.
         */
        CASE(15)
        li      s1, LOOP
        j       alias_a
        .balign 256
alias_a:
        rdcycle a1
        beq     zero, zero, 1f
        nop
1:      rdcycle a2
        j       alias_b
        .balign 256
alias_b:
        rdcycle a4
        beq     zero, zero, 2f
        nop
2:      rdcycle a5
        addi    s1, s1, -1
        bnez    s1, alias_a
        sub     a3, a2, a1
        CHECK(a3, TAKEN_COLD)
        sub     a3, a5, a4
        CHECK(a3, TAKEN_COLD)

TEST_END
