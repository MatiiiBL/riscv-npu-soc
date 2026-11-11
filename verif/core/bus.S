/*
 * SoC address decoding through the core's data port (docs/spec.md 3): the
 * MMIO registers, the edges of DMEM, and addresses that are not backed
 * (IMEM, the space past each region, unmapped regions), which read 0 and
 * drop writes. Only NPU register behavior required of every NPU is used.
 */
#include "test.h"

TEST_BEGIN
        li      s0, MMIO_BASE

        CASE(1)                         /* LEDS keeps the low byte and reads it back */
        li      a1, 0x1a5
        sw      a1, 0x20(s0)
        lw      a2, 0x20(s0)
        CHECK(a2, 0xa5)

        CASE(2)                         /* write-only registers read 0; status is ready */
        lw      a2, 0x00(s0)
        CHECK(a2, 0)
        lw      a2, 0x10(s0)
        CHECK(a2, 0)
        lw      a2, 0x04(s0)
        CHECK(a2, 1)

        CASE(3)                         /* undefined MMIO offsets read 0 */
        lw      a2, 0x08(s0)
        CHECK(a2, 0)
        li      a3, MMIO_BASE + 0xffc
        lw      a2, 0(a3)
        CHECK(a2, 0)

        CASE(4)                         /* the MMIO window does not repeat */
        li      a3, MMIO_BASE + 0x1000
        li      a1, 0x3c
        sw      a1, 0x20(a3)            /* would alias LEDS */
        lw      a2, 0x20(s0)
        CHECK(a2, 0xa5)
        lw      a2, 0x20(a3)
        CHECK(a2, 0)
        lw      a2, 0x04(a3)            /* would alias UART_STATUS */
        CHECK(a2, 0)

        CASE(5)                         /* DMEM: last word works, the next one is not backed */
        li      a3, DMEM_BASE + DMEM_SIZE - 4
        li      a1, 0x5eed
        sw      a1, 0(a3)
        lw      a2, 0(a3)
        CHECK(a2, 0x5eed)
        li      a4, DMEM_BASE
        lw      a5, 0(a4)
        li      a3, DMEM_BASE + DMEM_SIZE
        sw      a1, 0(a3)               /* would alias the first DMEM word */
        lw      a2, 0(a3)
        CHECK(a2, 0)
        lw      a2, 0(a4)
        bne     a2, a5, fail

        CASE(6)                         /* IMEM is not visible to loads and stores */
        li      a3, IMEM_BASE
        lw      a2, 0(a3)
        CHECK(a2, 0)
        sw      a1, 0(a3)

        CASE(7)                         /* unmapped regions */
        li      a3, 0x40000000
        sw      a1, 0(a3)
        lw      a2, 0(a3)
        CHECK(a2, 0)
        li      a3, 0xf0000000
        lw      a2, 0(a3)
        CHECK(a2, 0)

        CASE(8)                         /* NPU registers; the window does not repeat */
        li      a3, NPU_A_ADDR
        li      a1, 0x10000100
        sw      a1, 0(a3)
        lw      a2, 0(a3)
        CHECK(a2, 0x10000100)
        li      a3, NPU_QMULT
        lw      a2, 0(a3)
        li      a4, 0xffff
        and     a2, a2, a4
        CHECK(a2, 1)
        li      a3, NPU_A_ADDR + 0x1000
        lw      a2, 0(a3)
        CHECK(a2, 0)

TEST_END
