/*
 * Jumps and calls: link values, jalr with unusual registers and offsets,
 * nested calls with ra reloaded right before ret, a return target that
 * changes on every call, recursion, a jump table, chains of jumps and a tail
 * call. A wrong return address shows up as a bad count or a timeout.
 */
#include "test.h"

        .data
        .align  4
jtab:   .word   jt0, jt1, jt2, jt3, jt4, jt5, jt6, jt7

TEST_BEGIN
        CASE(1)                         /* jal link value, callee returns at once */
        la      a4, 1f
        jal     ra, f_ret
1:      bne     ra, a4, fail

        CASE(2)                         /* jalr linking into t0 */
        li      a5, 0
        la      a1, f_t0
        jalr    t0, 0(a1)
        CHECK(a5, 0x70)

        CASE(3)                         /* jalr with rd == rs1 */
        la      a1, f_ret_a1
        jalr    a1, 0(a1)
2:      la      a4, 2b
        bne     a1, a4, fail

        CASE(4)                         /* jalr clears bit 0 of the target */
        li      a5, 0
        la      a1, f_mark
        jalr    ra, 1(a1)
        CHECK(a5, 0x55)

        CASE(5)                         /* nested calls, ra on the stack */
        li      a5, 0
        call    f_lvl1
        CHECK(a5, 0x1234765)

        CASE(6)                         /* same callee, three call sites */
        li      a5, 0
        li      a6, 4
3:      call    f_inc
        slli    a5, a5, 1
        call    f_inc
        slli    a5, a5, 1
        call    f_inc
        addi    a6, a6, -1
        bnez    a6, 3b
        CHECK(a5, 595)                  /* a5 = 4 * a5 + 7, four times */

        CASE(7)                         /* recursion */
        li      a0, 15
        call    fib
        CHECK(a0, 610)

        CASE(8)                         /* jump table: load feeding jalr */
        li      a1, 0
        li      a5, 0
        li      a6, 8
4:      la      a2, jtab
        slli    a3, a1, 2
        add     a2, a2, a3
        lw      a3, 0(a2)
        jalr    ra, 0(a3)
        addi    a1, a1, 1
        blt     a1, a6, 4b
        CHECK(a5, 255)

        CASE(9)                         /* consecutive jumps */
        j       5f
        j       fail
5:      j       6f
        j       fail
6:      j       7f
        j       fail
7:

        CASE(10)                        /* tail call */
        li      a5, 0
        call    f_tail
        CHECK(a5, 3)

TEST_END

f_ret:
        ret

f_t0:
        li      a5, 0x70
        jr      t0

f_ret_a1:
        jr      a1

f_mark:
        li      a5, 0x55
        ret

f_inc:
        addi    a5, a5, 1
        ret

f_tail:
        addi    a5, a5, 2
        j       f_inc

/* Each level appends its number before calling deeper and number + 4 after. */
f_lvl1:
        addi    sp, sp, -16
        sw      ra, 0(sp)
        slli    a5, a5, 4
        addi    a5, a5, 1
        call    f_lvl2
        slli    a5, a5, 4
        addi    a5, a5, 5
        addi    sp, sp, 16
        lw      ra, -16(sp)
        ret                             /* load-use into jalr */
f_lvl2:
        addi    sp, sp, -16
        sw      ra, 0(sp)
        slli    a5, a5, 4
        addi    a5, a5, 2
        call    f_lvl3
        slli    a5, a5, 4
        addi    a5, a5, 6
        lw      ra, 0(sp)
        addi    sp, sp, 16
        ret
f_lvl3:
        addi    sp, sp, -16
        sw      ra, 0(sp)
        slli    a5, a5, 4
        addi    a5, a5, 3
        call    f_lvl4
        slli    a5, a5, 4
        addi    a5, a5, 7
        lw      ra, 0(sp)
        addi    sp, sp, 16
        ret
f_lvl4:
        slli    a5, a5, 4
        addi    a5, a5, 4
        ret

/* a0 = fib(a0), naive recursion. */
fib:
        li      t0, 2
        blt     a0, t0, 1f
        addi    sp, sp, -16
        sw      ra, 12(sp)
        sw      s1, 8(sp)
        sw      s2, 4(sp)
        mv      s1, a0
        addi    a0, a0, -1
        call    fib
        mv      s2, a0
        addi    a0, s1, -2
        call    fib
        add     a0, a0, s2
        lw      ra, 12(sp)
        lw      s1, 8(sp)
        lw      s2, 4(sp)
        addi    sp, sp, 16
1:      ret

jt0:    addi    a5, a5, 1
        ret
jt1:    addi    a5, a5, 2
        ret
jt2:    addi    a5, a5, 4
        ret
jt3:    addi    a5, a5, 8
        ret
jt4:    addi    a5, a5, 16
        ret
jt5:    addi    a5, a5, 32
        ret
jt6:    addi    a5, a5, 64
        ret
jt7:    addi    a5, a5, 128
        ret
