/*
 * Pulse SoC memory map. Plain #defines only so this header can be included
 * from C and from assembly (the riscv-tests environment uses it too).
 * Must stay in sync with docs/spec.md, rtl/soc/ and tools/iss/.
 */
#ifndef PULSE_SOC_MAP_H
#define PULSE_SOC_MAP_H

/* Instruction memory: fetch port only (Harvard). */
#define IMEM_BASE       0x00000000
#define IMEM_SIZE       0x00010000 /* 64 KiB */

/* Data memory: dual-ported, CPU load/store port + NPU DMA port. */
#define DMEM_BASE       0x10000000
#define DMEM_SIZE       0x00040000 /* 256 KiB */

/* Peripherals. All registers are 32-bit, word access only. */
#define MMIO_BASE       0x20000000
#define UART_TX         (MMIO_BASE + 0x00) /* W: low byte is transmitted   */
#define UART_STATUS     (MMIO_BASE + 0x04) /* R: bit0 = tx ready            */
#define SIM_EXIT        (MMIO_BASE + 0x10) /* W: end simulation, exit code  */
#define LEDS            (MMIO_BASE + 0x20) /* RW: board LEDs (low 8 bits)   */

/* NPU control/status registers (see docs/spec.md, section 5). */
#define NPU_BASE        0x30000000
#define NPU_CTRL        (NPU_BASE + 0x00)
#define NPU_STATUS      (NPU_BASE + 0x04)
#define NPU_A_ADDR      (NPU_BASE + 0x08)
#define NPU_B_ADDR      (NPU_BASE + 0x0C)
#define NPU_C_ADDR      (NPU_BASE + 0x10)
#define NPU_M           (NPU_BASE + 0x14)
#define NPU_N           (NPU_BASE + 0x18)
#define NPU_K           (NPU_BASE + 0x1C)
#define NPU_A_STRIDE    (NPU_BASE + 0x20)
#define NPU_B_STRIDE    (NPU_BASE + 0x24)
#define NPU_C_STRIDE    (NPU_BASE + 0x28)
#define NPU_QUANT       (NPU_BASE + 0x2C)
#define NPU_BIAS_ADDR   (NPU_BASE + 0x30)
#define NPU_CYCLES      (NPU_BASE + 0x34)
#define NPU_INFO        (NPU_BASE + 0x38)
#define NPU_QMULT       (NPU_BASE + 0x3C)

/* NPU_CTRL bits */
#define NPU_CTRL_START      (1u << 0)
#define NPU_CTRL_ACC_KEEP   (1u << 1) /* do not clear accumulators first */
#define NPU_CTRL_NO_WB      (1u << 2) /* do not write C back             */
#define NPU_CTRL_B_KEEP     (1u << 3) /* reuse B if loaded with the same B_ADDR/B_STRIDE/N/K */
#define NPU_CTRL_A_KEEP     (1u << 4) /* reuse A if loaded with the same A_ADDR/A_STRIDE/M/K */

/* NPU_STATUS bits */
#define NPU_STATUS_BUSY     (1u << 0)
#define NPU_STATUS_DONE     (1u << 1)
#define NPU_STATUS_ERR      (1u << 2)

/* NPU_QUANT fields */
#define NPU_QUANT_SHIFT(s)  ((unsigned)(s) & 0x1Fu) /* bits [4:0] */
#define NPU_QUANT_OUT_INT8  (1u << 8)
#define NPU_QUANT_RELU      (1u << 9)

#endif /* PULSE_SOC_MAP_H */
