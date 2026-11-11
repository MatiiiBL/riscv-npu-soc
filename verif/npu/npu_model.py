"""Reference models for the NPU testbench.

`Dmem` is a byte-level image of DMEM as seen from the DMA port. `NpuModel` is
the golden model of docs/spec.md section 5.1: it holds the architectural state
(registers, status, the ARR x ARR accumulators) and executes one operation at a
time against a `Dmem`, bit-exactly. It is deliberately written straight from
the spec pseudocode and shares no code with the hardware's structure.
"""

from __future__ import annotations

import numpy as np

DMEM_BASE = 0x1000_0000
DMEM_SIZE = 256 * 1024
MASK32 = 0xFFFF_FFFF

# Register offsets (docs/spec.md section 5).
CTRL = 0x00
STATUS = 0x04
A_ADDR = 0x08
B_ADDR = 0x0C
C_ADDR = 0x10
M = 0x14
N = 0x18
K = 0x1C
A_STRIDE = 0x20
B_STRIDE = 0x24
C_STRIDE = 0x28
QUANT = 0x2C
BIAS_ADDR = 0x30
CYCLES = 0x34
INFO = 0x38
QMULT = 0x3C

RW_REGS = (A_ADDR, B_ADDR, C_ADDR, M, N, K, A_STRIDE, B_STRIDE, C_STRIDE,
           QUANT, BIAS_ADDR, QMULT)
REG_NAMES = {
    CTRL: "CTRL", STATUS: "STATUS", A_ADDR: "A_ADDR", B_ADDR: "B_ADDR",
    C_ADDR: "C_ADDR", M: "M", N: "N", K: "K", A_STRIDE: "A_STRIDE",
    B_STRIDE: "B_STRIDE", C_STRIDE: "C_STRIDE", QUANT: "QUANT",
    BIAS_ADDR: "BIAS_ADDR", CYCLES: "CYCLES", INFO: "INFO", QMULT: "QMULT",
}

CTRL_START = 1 << 0
CTRL_ACC_KEEP = 1 << 1
CTRL_NO_WB = 1 << 2

STATUS_BUSY = 1 << 0
STATUS_DONE = 1 << 1
STATUS_ERR = 1 << 2

QUANT_OUT_INT8 = 1 << 8
QUANT_RELU = 1 << 9


def s8(x: int) -> int:
    x &= 0xFF
    return x - 0x100 if x & 0x80 else x


def s16(x: int) -> int:
    x &= 0xFFFF
    return x - 0x1_0000 if x & 0x8000 else x


def s32(x: int) -> int:
    x &= MASK32
    return x - 0x1_0000_0000 if x & 0x8000_0000 else x


def quant(shift: int = 0, out_int8: bool = False, relu: bool = False) -> int:
    """Encode a QUANT register value."""
    return (shift & 0x1F) | (QUANT_OUT_INT8 if out_int8 else 0) | (QUANT_RELU if relu else 0)


class Dmem:
    """DMEM image addressed with full SoC byte addresses.

    Accesses outside DMEM raise: the testbench never places operands there,
    so such an access is an NPU bug.
    """

    def __init__(self, data: bytes | bytearray | None = None):
        self.data = bytearray(data) if data is not None else bytearray(DMEM_SIZE)

    def copy(self) -> Dmem:
        return Dmem(self.data)

    def offset(self, addr: int, size: int = 1) -> int:
        off = (addr - DMEM_BASE) & MASK32
        if off + size > DMEM_SIZE:
            raise IndexError(f"access to 0x{addr & MASK32:08x} is outside DMEM")
        return off

    def read(self, addr: int, size: int) -> bytes:
        off = self.offset(addr, size)
        return bytes(self.data[off:off + size])

    def write(self, addr: int, payload: bytes) -> None:
        off = self.offset(addr, len(payload))
        self.data[off:off + len(payload)] = payload

    def read32(self, addr: int) -> int:
        return int.from_bytes(self.read(addr, 4), "little")

    def write32(self, addr: int, value: int) -> None:
        self.write(addr, (value & MASK32).to_bytes(4, "little"))

    def write_be(self, addr: int, value: int, be: int, size: int = 4) -> None:
        """A bus write of `size` bytes with byte enables (lane i = byte addr + i)."""
        off = self.offset(addr, size)
        for lane in range(size):
            if be & (1 << lane):
                self.data[off + lane] = (value >> (8 * lane)) & 0xFF


class NpuModel:
    """Architectural model of npu_top, executing operations per spec 5.1."""

    def __init__(self, arr: int, kmax: int):
        self.arr = arr
        self.kmax = kmax
        self.reset()

    def reset(self) -> None:
        self.regs = {off: 0 for off in RW_REGS}
        self.regs[QMULT] = 1
        self.status = 0
        self.acc = [[0] * self.arr for _ in range(self.arr)]

    @property
    def info(self) -> int:
        return (1 << 24) | (self.kmax << 8) | self.arr

    def read_reg(self, off: int) -> int:
        """Register value as the hardware reports it (CYCLES excluded)."""
        if off in self.regs:
            return self.regs[off]
        if off == STATUS:
            return self.status
        if off == INFO:
            return self.info
        return 0

    def params_ok(self) -> bool:
        r = self.regs
        shape = (1 <= r[M] <= self.arr and 1 <= r[N] <= self.arr
                 and 1 <= r[K] <= self.kmax)
        aligned = all(r[x] % 4 == 0 for x in (A_ADDR, B_ADDR, C_ADDR, BIAS_ADDR,
                                              A_STRIDE, B_STRIDE, C_STRIDE))
        return shape and aligned

    def _matrix(self, mem: Dmem, base: int, stride: int, rows: int, k: int) -> np.ndarray:
        out = np.empty((rows, k), dtype=np.int64)
        for row in range(rows):
            raw = mem.read((base + row * stride) & MASK32, k)
            out[row] = np.frombuffer(raw, dtype=np.int8)
        return out

    def start(self, ctrl: int, mem: Dmem) -> None:
        """A CTRL write with START set, issued while the NPU is idle."""
        r = self.regs
        if not self.params_ok():
            self.status = STATUS_ERR | STATUS_DONE
            return

        if not ctrl & CTRL_ACC_KEEP:
            self.acc = [[0] * self.arr for _ in range(self.arr)]

        m_rows, n_rows, k_len = r[M], r[N], r[K]
        a = self._matrix(mem, r[A_ADDR], r[A_STRIDE], m_rows, k_len)
        b = self._matrix(mem, r[B_ADDR], r[B_STRIDE], n_rows, k_len)
        dot = a @ b.T
        for m in range(m_rows):
            for n in range(n_rows):
                self.acc[m][n] = s32(self.acc[m][n] + int(dot[m, n]))

        if not ctrl & CTRL_NO_WB:
            self._write_back(mem)
        self.status = STATUS_DONE

    def _write_back(self, mem: Dmem) -> None:
        r = self.regs
        shift = r[QUANT] & 0x1F
        out_int8 = bool(r[QUANT] & QUANT_OUT_INT8)
        relu = bool(r[QUANT] & QUANT_RELU)
        qmult = s16(r[QMULT])
        for m in range(r[M]):
            row = (r[C_ADDR] + m * r[C_STRIDE]) & MASK32
            for n in range(r[N]):
                bias = s32(mem.read32((r[BIAS_ADDR] + 4 * n) & MASK32)) if r[BIAS_ADDR] else 0
                v = s32(self.acc[m][n] + bias)
                if out_int8:
                    p = v * qmult
                    if shift > 0:
                        p = (p + (1 << (shift - 1))) >> shift
                    if relu and p < 0:
                        p = 0
                    p = max(-128, min(127, p))
                    mem.write((row + n) & MASK32, bytes([p & 0xFF]))
                else:
                    if relu and v < 0:
                        v = 0
                    mem.write32((row + 4 * n) & MASK32, v)


def row_blocks(base: int, length: int, dma_bytes: int) -> list[int]:
    """Aligned DMA blocks covering `length` bytes from `base`."""
    first = base & ~(dma_bytes - 1)
    last = (base + length - 1) & ~(dma_bytes - 1)
    return [(first + i) & MASK32 for i in range(0, last - first + 1, dma_bytes)]


def expected_reads(regs: dict[int, int], ctrl: int, dma_bytes: int) -> list[int]:
    """Block addresses the DMA reads for an operation, in issue order."""
    addrs = []
    for base, stride, rows in ((regs[A_ADDR], regs[A_STRIDE], regs[M]),
                               (regs[B_ADDR], regs[B_STRIDE], regs[N])):
        for row in range(rows):
            addrs += row_blocks(base + row * stride, regs[K], dma_bytes)
    if regs[BIAS_ADDR] and not ctrl & CTRL_NO_WB:
        addrs += row_blocks(regs[BIAS_ADDR], 4 * regs[N], dma_bytes)
    return addrs


def expected_cycles(regs: dict[int, int], ctrl: int, dma_bytes: int) -> int:
    """CYCLES of a valid operation: the four phases of docs/npu.md."""
    m, n, k = regs[M], regs[N], regs[K]
    load = len(expected_reads(regs, ctrl, dma_bytes)) + 1
    compute = k + m + n + 2
    write_back = 0 if ctrl & CTRL_NO_WB else m * n + 6
    return load + compute + write_back
