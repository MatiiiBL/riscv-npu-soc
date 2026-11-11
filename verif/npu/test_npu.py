"""cocotb tests for npu_top.

Every operation is checked against the golden model in npu_model.py:
  - the whole 256 KiB DMEM image after the operation, byte for byte (so stray
    or clobbering writes anywhere are caught, not only wrong C values),
  - STATUS during and after the operation, and irq,
  - CYCLES against the phase formula of docs/npu.md,
  - the exact sequence of DMA read addresses.

The DMEM model answers reads with one cycle of latency and drives random data
on dma_rdata in every cycle that carries no read response, so any use of the
bus data at the wrong time shows up as a mismatch.

Configuration comes from the environment (set by run.py): NPU_ARR, NPU_KMAX,
NPU_SEED (default 1), NPU_RANDOM_OPS (default 300) and NPU_REPORT (a file the
cycle measurements are written to).
"""

from __future__ import annotations

import os
import random
from dataclasses import dataclass, field

import cocotb
import numpy as np
from cocotb.clock import Clock
from cocotb.triggers import FallingEdge

from npu_model import (A_ADDR, A_STRIDE, B_ADDR, B_STRIDE, BIAS_ADDR, C_ADDR,
                       C_STRIDE, CTRL, CTRL_ACC_KEEP, CTRL_NO_WB, CTRL_START,
                       CYCLES, DMEM_BASE, DMEM_SIZE, INFO, MASK32, QMULT,
                       QUANT, REG_NAMES, RW_REGS, STATUS, STATUS_BUSY,
                       STATUS_DONE, STATUS_ERR, Dmem, K, M, N, NpuModel,
                       expected_cycles, expected_reads, quant, s8, s16, s32)

ARR = int(os.environ.get("NPU_ARR", "8"))
KMAX = int(os.environ.get("NPU_KMAX", "1024"))
SEED = int(os.environ.get("NPU_SEED", "1"))
RANDOM_OPS = int(os.environ.get("NPU_RANDOM_OPS", "300"))

# Operands live in separate quarters of DMEM so regions never overlap.
QUARTER = DMEM_SIZE // 4
A_REGION, B_REGION, C_REGION, BIAS_REGION = (DMEM_BASE + i * QUARTER for i in range(4))


def round4(x: int) -> int:
    return (x + 3) & ~3


@dataclass
class Op:
    """One NPU operation: register values plus the operand data to place."""

    a: np.ndarray                       # M x K int8
    b: np.ndarray                       # N x K int8
    bias: np.ndarray | None = None      # N int32, or None for BIAS_ADDR = 0
    a_addr: int = A_REGION
    b_addr: int = B_REGION
    c_addr: int = C_REGION
    bias_addr: int = BIAS_REGION
    a_stride: int | None = None         # default: dense rows
    b_stride: int | None = None
    c_stride: int | None = None
    quant: int = 0
    qmult: int = 1
    ctrl: int = 0
    regs: dict[int, int] = field(default_factory=dict)

    def __post_init__(self):
        m, k = self.a.shape
        n = self.b.shape[0]
        out_bytes = n if self.quant & (1 << 8) else 4 * n
        self.a_stride = round4(k) if self.a_stride is None else self.a_stride
        self.b_stride = round4(k) if self.b_stride is None else self.b_stride
        self.c_stride = round4(out_bytes) if self.c_stride is None else self.c_stride
        self.regs = {
            A_ADDR: self.a_addr, B_ADDR: self.b_addr, C_ADDR: self.c_addr,
            M: m, N: n, K: k,
            A_STRIDE: self.a_stride, B_STRIDE: self.b_stride, C_STRIDE: self.c_stride,
            QUANT: self.quant, QMULT: self.qmult & MASK32,
            BIAS_ADDR: self.bias_addr if self.bias is not None else 0,
        }

    def describe(self) -> str:
        r = self.regs
        return (f"M={r[M]} N={r[N]} K={r[K]} ctrl={self.ctrl:#x} quant={r[QUANT]:#x} "
                f"qmult={s16(r[QMULT])} bias={'yes' if self.bias is not None else 'no'} "
                f"strides={r[A_STRIDE]}/{r[B_STRIDE]}/{r[C_STRIDE]}")


class Testbench:
    def __init__(self, dut, name: str):
        self.dut = dut
        self.rng = random.Random(f"{SEED}-{name}")
        self.np_rng = np.random.default_rng(self.rng.getrandbits(32))
        self.junk = random.Random(self.rng.getrandbits(32))
        self.mem = Dmem(self.rng.randbytes(DMEM_SIZE))
        self.model = NpuModel(ARR, KMAX)
        self.reads: list[int] = []
        self.writes: list[tuple[int, int, int]] = []
        self.irq_at_read = 0
        dut._log.info("seed=%d ARR=%d KMAX=%d", SEED, ARR, KMAX)

    @classmethod
    async def create(cls, dut, name: str) -> Testbench:
        tb = cls(dut, name)
        await tb.reset()
        return tb

    async def reset(self) -> None:
        d = self.dut
        Clock(d.clk, 10, unit="ns").start()
        d.rst_n.value = 0
        d.cfg_req.value = 0
        d.cfg_we.value = 0
        d.cfg_addr.value = 0
        d.cfg_wdata.value = 0
        d.dma_rdata.value = 0
        for _ in range(4):
            await FallingEdge(d.clk)
        d.rst_n.value = 1
        self.model.reset()
        cocotb.start_soon(self._dmem())

    async def _dmem(self) -> None:
        """DMEM port B: sample the request mid-cycle, answer reads next cycle."""
        d = self.dut
        resp = None
        while True:
            await FallingEdge(d.clk)
            d.dma_rdata.value = resp if resp is not None else self.junk.getrandbits(32)
            resp = None
            if int(d.dma_req.value):
                addr = int(d.dma_addr.value)
                assert addr % 4 == 0, f"misaligned DMA address 0x{addr:08x}"
                if int(d.dma_we.value):
                    be, data = int(d.dma_be.value), int(d.dma_wdata.value)
                    assert be != 0, f"DMA write with no byte enables at 0x{addr:08x}"
                    self.mem.write_be(addr, data, be)
                    self.writes.append((addr, be, data))
                else:
                    resp = self.mem.read32(addr)
                    self.reads.append(addr)

    # ---------------------------------------------------------------- bus

    async def write(self, off: int, value: int) -> None:
        d = self.dut
        await FallingEdge(d.clk)
        d.cfg_req.value = 1
        d.cfg_we.value = 1
        d.cfg_addr.value = off
        d.cfg_wdata.value = value & MASK32
        await FallingEdge(d.clk)
        d.cfg_req.value = 0
        d.cfg_we.value = 0

    async def read(self, off: int) -> int:
        d = self.dut
        await FallingEdge(d.clk)
        d.cfg_req.value = 1
        d.cfg_we.value = 0
        d.cfg_addr.value = off
        # irq in the cycle the register is sampled, for read_status().
        self.irq_at_read = int(d.irq.value)
        await FallingEdge(d.clk)
        d.cfg_req.value = 0
        return int(d.cfg_rdata.value)

    async def read_status(self) -> int:
        """Read STATUS and check that irq agreed with DONE in the same cycle."""
        status = await self.read(STATUS)
        assert self.irq_at_read == bool(status & STATUS_DONE), (
            f"irq={self.irq_at_read} with STATUS={status:#x}")
        return status

    async def idle(self, cycles: int) -> None:
        for _ in range(cycles):
            await FallingEdge(self.dut.clk)

    async def configure(self, regs: dict[int, int]) -> None:
        for off, value in regs.items():
            await self.write(off, value)
            self.model.regs[off] = value & MASK32

    # ----------------------------------------------------------- operations

    def place(self, op: Op) -> None:
        for base, stride, mat in ((op.a_addr, op.a_stride, op.a), (op.b_addr, op.b_stride, op.b)):
            for row in range(mat.shape[0]):
                self.mem.write(base + row * stride, mat[row].astype(np.int8).tobytes())
        if op.bias is not None:
            self.mem.write(op.bias_addr, op.bias.astype("<i4").tobytes())

    async def run_op(self, op: Op) -> int:
        self.place(op)
        await self.configure(op.regs)
        return await self.start(op.ctrl, what=op.describe())

    async def start(self, ctrl: int, what: str = "", during=None) -> int:
        """START with the current registers; check the result. Returns CYCLES.

        `during`, if given, is awaited right after the START write, before the
        first STATUS read (which must then still report BUSY).
        """
        d = self.dut
        regs = dict(self.model.regs)
        golden = self.mem.copy()
        self.model.start(ctrl, golden)
        valid = not self.model.status & STATUS_ERR
        self.reads.clear()
        self.writes.clear()

        await self.write(CTRL, CTRL_START | ctrl)
        if during is not None:
            await during()
        status = await self.read_status()
        expect_first = STATUS_BUSY if valid else STATUS_ERR | STATUS_DONE
        assert status == expect_first, f"STATUS right after START = {status:#x} ({what})"

        polls = 0
        while not status & STATUS_DONE:
            polls += 1
            assert polls < 50_000, f"operation never finished ({what})"
            status = await self.read_status()
        assert status == self.model.status, f"final STATUS {status:#x} ({what})"
        assert int(d.irq.value) == 1, "irq low after DONE"

        cycles = await self.read(CYCLES)
        if valid:
            assert cycles == expected_cycles(regs, ctrl), (
                f"CYCLES {cycles} != {expected_cycles(regs, ctrl)} ({what})")
            assert self.reads == expected_reads(regs, ctrl), f"DMA read sequence ({what})"
        else:
            assert cycles == 0, f"CYCLES {cycles} after a rejected operation"
            assert not self.reads and not self.writes, f"memory traffic on ERR ({what})"
        self.check_memory(golden, what)
        return cycles

    def check_memory(self, golden: Dmem, what: str) -> None:
        if self.mem.data == golden.data:
            return
        diffs = [i for i in range(DMEM_SIZE) if self.mem.data[i] != golden.data[i]]
        lines = [f"0x{DMEM_BASE + i:08x}: got {self.mem.data[i]:02x} want {golden.data[i]:02x}"
                 for i in diffs[:16]]
        raise AssertionError(f"{len(diffs)} DMEM bytes differ ({what}):\n  " + "\n  ".join(lines))

    # ------------------------------------------------------------- stimulus

    def matrix(self, rows: int, k: int, kind: str = "full") -> np.ndarray:
        if kind == "full":
            return self.np_rng.integers(-128, 128, size=(rows, k), dtype=np.int64)
        if kind == "small":
            return self.np_rng.integers(-4, 5, size=(rows, k), dtype=np.int64)
        if kind == "extreme":
            return self.np_rng.choice(np.array([-128, -127, 127, 0, -1, 1]), size=(rows, k))
        if kind == "max":
            return np.full((rows, k), -128, dtype=np.int64)
        raise ValueError(kind)

    def addr_in(self, region: int, size: int) -> int:
        """A random word-aligned address in a DMEM quarter with room for size bytes."""
        assert size <= QUARTER
        return region + 4 * self.rng.randrange((QUARTER - size) // 4 + 1)

    def random_op(self, ctrl: int | None = None) -> Op:
        rng = self.rng
        m, n = rng.randint(1, ARR), rng.randint(1, ARR)
        k = rng.choice([rng.randint(1, 8), rng.randint(1, 64), rng.randint(1, KMAX),
                        rng.choice([KMAX - 1, KMAX])])
        k = max(1, min(k, KMAX))
        kind = rng.choice(["full", "full", "small", "extreme"])
        a, b = self.matrix(m, k, kind), self.matrix(n, k, kind)
        bias = None
        if rng.random() < 0.5:
            span = rng.choice([1 << 8, 1 << 20, 1 << 31])
            bias = self.np_rng.integers(-span, span, size=n, dtype=np.int64)
        out_int8, relu = rng.random() < 0.6, rng.random() < 0.4
        qmult = rng.choice([1, -1, 32767, -32768, rng.randint(-32768, 32767),
                            rng.randint(1, 255)])
        # Pick SHIFT around the magnitude of v * QMULT so the INT8 results mix
        # in-range values with saturation at both ends.
        vmax = int(np.abs(a @ b.T).max()) + (int(np.abs(bias).max()) if bias is not None else 0)
        shift = (vmax * abs(qmult)).bit_length() - 7 + rng.randint(-3, 3)
        shift = rng.choice([max(0, min(31, shift)), 0, 1, 31, rng.randint(0, 31)])
        if ctrl is None:
            ctrl = rng.choice([0, 0, 0, CTRL_ACC_KEEP, CTRL_NO_WB, CTRL_ACC_KEEP | CTRL_NO_WB])
        out_bytes = n if out_int8 else 4 * n

        def stride(min_len: int) -> int:
            pick = rng.random()
            if pick < 0.05:
                return 0
            if pick < 0.6:
                return round4(min_len)
            return round4(min_len) + 4 * rng.randint(1, 64)

        a_stride, b_stride, c_stride = stride(k), stride(k), stride(out_bytes)
        return Op(a=a, b=b, bias=bias,
                  a_addr=self.addr_in(A_REGION, m * max(a_stride, round4(k))),
                  b_addr=self.addr_in(B_REGION, n * max(b_stride, round4(k))),
                  c_addr=self.addr_in(C_REGION, m * max(c_stride, round4(out_bytes))),
                  bias_addr=self.addr_in(BIAS_REGION, 4 * n),
                  a_stride=a_stride, b_stride=b_stride, c_stride=c_stride,
                  quant=quant(shift, out_int8, relu), qmult=qmult, ctrl=ctrl)


def gemm_reference(a: np.ndarray, b: np.ndarray, bias: np.ndarray | None,
                   q: int, qmult: int) -> list[list[int]]:
    """C = requant(A * B^T + bias) for a whole problem, independent of NpuModel."""
    shift, out_int8, relu = q & 0x1F, bool(q & (1 << 8)), bool(q & (1 << 9))
    acc = (a @ b.T).tolist()
    out = []
    for row in acc:
        vals = []
        for n, x in enumerate(row):
            v = s32(x + (int(bias[n]) if bias is not None else 0))
            if out_int8:
                p = v * s16(qmult)
                if shift:
                    p = (p + (1 << (shift - 1))) >> shift
                v = max(-128, min(127, max(0, p) if relu else p))
            elif relu:
                v = max(0, v)
            vals.append(v)
        out.append(vals)
    return out


# ===================================================================== tests


@cocotb.test()
async def test_reset_values(dut):
    """All registers reset to 0 except QMULT = 1; INFO reports ARR and KMAX."""
    tb = await Testbench.create(dut, "reset")
    assert int(dut.irq.value) == 0
    for off in range(0, 0x40, 4):
        value = await tb.read(off)
        assert value == tb.model.read_reg(off), f"{REG_NAMES[off]} = {value:#x} after reset"
    info = await tb.read(INFO)
    assert info == (1 << 24) | (KMAX << 8) | ARR, f"INFO = {info:#010x}"
    assert info & 0xFF == ARR and (info >> 8) & 0xFFFF == KMAX and info >> 24 == 1
    for off in (0x40, 0x44, 0x7FC, 0x800, 0xFFC):
        assert await tb.read(off) == 0, f"undefined offset {off:#x} reads nonzero"
    assert not tb.reads and not tb.writes


@cocotb.test()
async def test_register_readback(dut):
    """RW registers return the full 32-bit value; RO and undefined writes drop."""
    tb = await Testbench.create(dut, "readback")
    for _ in range(4):
        values = {off: tb.rng.getrandbits(32) for off in RW_REGS}
        await tb.configure(values)
        for off, want in values.items():
            got = await tb.read(off)
            assert got == want, f"{REG_NAMES[off]} read {got:#x}, wrote {want:#x}"

    snapshot = {off: await tb.read(off) for off in range(0, 0x40, 4)}
    for off in (STATUS, CYCLES, INFO, 0x40, 0x100, 0xFFC):
        await tb.write(off, tb.rng.getrandbits(32))
    await tb.write(CTRL, 0xFFFF_FFFE)          # every bit but START: no operation
    await tb.idle(8)
    for off in range(0, 0x40, 4):
        got = await tb.read(off)
        assert got == snapshot[off], f"{REG_NAMES[off]} changed to {got:#x}"
    assert await tb.read(CTRL) == 0
    assert await tb.read(STATUS) == 0
    assert not tb.reads and not tb.writes, "a CTRL write without START moved data"


@cocotb.test()
async def test_known_values(dut):
    """A small hand-checkable product, INT32 out with bias."""
    tb = await Testbench.create(dut, "known")
    m, n = min(2, ARR), min(3, ARR)
    a = np.array([[1, 2, 3, 4, 5], [-1, -2, -3, -4, -5]], dtype=np.int64)[:m]
    b = np.array([[1, 0, 0, 0, 0], [1, 1, 1, 1, 1], [-128, 127, -128, 127, 2]], dtype=np.int64)[:n]
    bias = np.array([100, -100, 7], dtype=np.int64)[:n]
    await tb.run_op(Op(a=a, b=b, bias=bias))
    want = [[101, -85, 267], [99, -115, -253]]
    for i in range(m):
        for j in range(n):
            got = s32(tb.mem.read32(C_REGION + 4 * (n * i + j)))
            assert got == want[i][j], f"C[{i}][{j}] = {got}, want {want[i][j]}"


@cocotb.test()
async def test_shapes(dut):
    """Every (M, N) and the K corner cases, INT32 and INT8 output."""
    tb = await Testbench.create(dut, "shapes")
    for m in range(1, ARR + 1):
        for n in range(1, ARR + 1):
            k = tb.rng.randint(1, 9)
            await tb.run_op(Op(a=tb.matrix(m, k), b=tb.matrix(n, k)))
    ks = [1, 2, 3, 4, 5, 6, 7, 8, 9, 13, 64, 255, KMAX - 3, KMAX - 2, KMAX - 1, KMAX]
    for k in sorted({max(1, min(k, KMAX)) for k in ks}):
        for out_int8 in (False, True):
            m, n = tb.rng.randint(1, ARR), tb.rng.randint(1, ARR)
            await tb.run_op(Op(a=tb.matrix(m, k), b=tb.matrix(n, k),
                               quant=quant(shift=10, out_int8=out_int8)))
    await tb.run_op(Op(a=tb.matrix(ARR, KMAX, "max"), b=tb.matrix(ARR, KMAX, "max")))


@cocotb.test()
async def test_relu_bias(dut):
    """Bias add and ReLU in both output formats."""
    tb = await Testbench.create(dut, "relu_bias")
    for out_int8 in (False, True):
        for relu in (False, True):
            for span in (1 << 10, 1 << 31):
                m, n, k = ARR, ARR, tb.rng.randint(1, 100)
                bias = tb.np_rng.integers(-span, span, size=n, dtype=np.int64)
                await tb.run_op(Op(a=tb.matrix(m, k), b=tb.matrix(n, k), bias=bias,
                                   quant=quant(12, out_int8, relu), qmult=3))


@cocotb.test()
async def test_requant_edges(dut):
    """Rounding, shifts 0/1/31, QMULT extremes and saturation at both ends.

    A is all zeros, so v = bias[n] exactly: each output lane exercises one
    chosen (v, QMULT, SHIFT) case.
    """
    tb = await Testbench.create(dut, "requant_edges")
    vs = [0, 1, -1, 2, -2, 3, -3, 5, -5, 127, 128, -128, -129, 255, -256,
          (1 << 31) - 1, -(1 << 31), 1 << 30, -(1 << 30), 0x1234_5678, -0x1234_5678]
    qmults = [1, -1, 2, 32767, -32768, -32767, 12345, -3]
    shifts = [0, 1, 2, 7, 15, 16, 30, 31]
    for qmult in qmults:
        for shift in shifts:
            for relu in (False, True):
                for start in range(0, len(vs), ARR):
                    lane_vs = vs[start:start + ARR]
                    n = len(lane_vs)
                    await tb.run_op(Op(a=np.zeros((1, 1), dtype=np.int64),
                                       b=tb.matrix(n, 1),
                                       bias=np.array(lane_vs, dtype=np.int64),
                                       quant=quant(shift, True, relu), qmult=qmult))
    # QMULT's upper half is ignored by the arithmetic but still read back.
    await tb.run_op(Op(a=tb.matrix(ARR, 32), b=tb.matrix(ARR, 32),
                       quant=quant(9, True), qmult=0xBEEF_FFFD))


@cocotb.test()
async def test_requant_random(dut):
    """Random products through random QMULT (negative and large) and SHIFT."""
    tb = await Testbench.create(dut, "requant_random")
    for i in range(40):
        m, n, k = tb.rng.randint(1, ARR), tb.rng.randint(1, ARR), tb.rng.randint(1, KMAX)
        qmult = tb.rng.choice([tb.rng.randint(-32768, -1), tb.rng.randint(16384, 32767),
                               -32768, 32767])
        shift = tb.rng.choice([0, 1, 31, tb.rng.randint(2, 30)])
        bias = tb.np_rng.integers(-(1 << 20), 1 << 20, size=n, dtype=np.int64) if i % 2 else None
        await tb.run_op(Op(a=tb.matrix(m, k), b=tb.matrix(n, k), bias=bias,
                           quant=quant(shift, True, i % 3 == 0), qmult=qmult))


@cocotb.test()
async def test_strides(dut):
    """Row strides larger than the row length (and zero) on A, B and C."""
    tb = await Testbench.create(dut, "strides")
    for i in range(16):
        m, n, k = ARR, tb.rng.randint(1, ARR), tb.rng.randint(1, 200)
        out_int8 = bool(i & 1)
        out_bytes = n if out_int8 else 4 * n
        strides = [round4(k) + 4 * tb.rng.randint(1, 300), round4(k) + 4 * tb.rng.randint(1, 300),
                   round4(out_bytes) + 4 * tb.rng.randint(1, 300)]
        if i == 14:
            strides[0] = strides[1] = 0         # every row reads the same data
        if i == 15:
            strides[2] = 0                      # rows overwrite each other in order
        await tb.run_op(Op(a=tb.matrix(m, k), b=tb.matrix(n, k),
                           a_stride=strides[0], b_stride=strides[1], c_stride=strides[2],
                           quant=quant(8, out_int8)))


@cocotb.test()
async def test_k_split(dut):
    """NO_WB, ACC_KEEP|NO_WB, ACC_KEEP over K chunks equals one long product."""
    tb = await Testbench.create(dut, "k_split")
    for out_int8 in (False, True):
        m, n = ARR, tb.rng.randint(1, ARR)
        chunks = [KMAX, tb.rng.randint(1, KMAX), tb.rng.randint(1, 9)]
        a = tb.matrix(m, sum(chunks))
        b = tb.matrix(n, sum(chunks))
        bias = tb.np_rng.integers(-1000, 1000, size=n, dtype=np.int64)
        q, qmult = quant(14, out_int8, relu=True), 77
        flags = [CTRL_NO_WB, CTRL_ACC_KEEP | CTRL_NO_WB, CTRL_ACC_KEEP]
        k0 = 0
        for k, ctrl in zip(chunks, flags):
            await tb.run_op(Op(a=a[:, k0:k0 + k], b=b[:, k0:k0 + k], bias=bias,
                               quant=q, qmult=qmult, ctrl=ctrl))
            k0 += k
        want = gemm_reference(a, b, bias, q, qmult)
        stride = round4(n if out_int8 else 4 * n)
        for i in range(m):
            for j in range(n):
                if out_int8:
                    got = s8(tb.mem.read(C_REGION + i * stride + j, 1)[0])
                else:
                    got = s32(tb.mem.read32(C_REGION + i * stride + 4 * j))
                assert got == want[i][j], f"C[{i}][{j}] = {got}, want {want[i][j]}"


@cocotb.test()
async def test_acc_keep_partial(dut):
    """ACC_KEEP on a smaller tile leaves the other accumulators untouched."""
    tb = await Testbench.create(dut, "acc_keep_partial")
    full = Op(a=tb.matrix(ARR, 33), b=tb.matrix(ARR, 33), ctrl=CTRL_NO_WB)
    await tb.run_op(full)
    for m, n in ((1, 1), (ARR, 1), (1, ARR), (max(1, ARR - 1), max(1, ARR // 2))):
        k = tb.rng.randint(1, 70)
        await tb.run_op(Op(a=tb.matrix(m, k), b=tb.matrix(n, k), ctrl=CTRL_ACC_KEEP | CTRL_NO_WB))
    # Read every accumulator out through a K = 1 product of zeros.
    await tb.run_op(Op(a=np.zeros((ARR, 1), dtype=np.int64), b=tb.matrix(ARR, 1),
                       ctrl=CTRL_ACC_KEEP))


@cocotb.test()
async def test_errors(dut):
    """Every invalid parameter gives ERR|DONE, no memory traffic, and keeps acc."""
    tb = await Testbench.create(dut, "errors")
    # Leave known values in the accumulators, then check they survive.
    base = Op(a=tb.matrix(ARR, 40), b=tb.matrix(ARR, 40), bias=np.arange(ARR), ctrl=CTRL_NO_WB)
    await tb.run_op(base)
    good = dict(base.regs)
    bad_values = {
        M: [0, ARR + 1, 0x100 + ARR, 0x8000_0001, MASK32],
        N: [0, ARR + 1, 0x1_0000 + 1, MASK32],
        K: [0, KMAX + 1, 0x1_0000 + 4, 0x8000_0000 + KMAX],
        A_ADDR: [good[A_ADDR] + 1, good[A_ADDR] + 2, good[A_ADDR] + 3],
        B_ADDR: [good[B_ADDR] + 2],
        C_ADDR: [good[C_ADDR] + 3],
        BIAS_ADDR: [1, good[BIAS_ADDR] + 2],
        A_STRIDE: [good[A_STRIDE] + 1],
        B_STRIDE: [good[B_STRIDE] + 2],
        C_STRIDE: [good[C_STRIDE] + 3],
    }
    for off, values in bad_values.items():
        for value in values:
            await tb.configure({off: value})
            for ctrl in (0, CTRL_ACC_KEEP):
                await tb.start(ctrl, what=f"{REG_NAMES[off]}={value:#x}")
            await tb.configure({off: good[off]})
    # The accumulators still hold the first product: finish it with K = 1 of zeros.
    await tb.run_op(Op(a=np.zeros((ARR, 1), dtype=np.int64), b=tb.matrix(ARR, 1),
                       bias=np.arange(ARR), ctrl=CTRL_ACC_KEEP))
    # ERR clears on the next START.
    await tb.run_op(Op(a=tb.matrix(1, 1), b=tb.matrix(1, 1)))


@cocotb.test()
async def test_busy_writes(dut):
    """Register writes while BUSY, START included, are ignored in every phase."""
    tb = await Testbench.create(dut, "busy_writes")
    m = n = ARR
    k = min(256, KMAX)
    probe = Op(a=tb.matrix(m, k), b=tb.matrix(n, k), bias=np.arange(n))
    load = len(expected_reads(probe.regs, 0)) + 1
    compute = k + m + n + 1
    write_back = m * n + 6
    # START first: the write that matters most. Then everything else.
    junk = [(CTRL, CTRL_START | CTRL_ACC_KEEP)]
    junk += [(off, tb.rng.getrandbits(32)) for off in RW_REGS]
    junk += [(M, 0), (CTRL, CTRL_START), (STATUS, MASK32), (CYCLES, 0), (INFO, 0)]
    # Each write takes two cycles and the STATUS read that follows must still
    # see BUSY, so a burst of n writes needs a phase of at least 2n + 2 cycles.
    for delay, length in ((0, load), (load, compute), (load + compute, write_back)):
        writes = junk[:max(1, (length - 2) // 2)]
        op = Op(a=tb.matrix(m, k), b=tb.matrix(n, k), bias=tb.np_rng.integers(-99, 99, size=n),
                quant=quant(9, True), qmult=5)
        tb.place(op)
        await tb.configure(op.regs)
        before = {off: await tb.read(off) for off in RW_REGS}

        async def hammer():
            await tb.idle(delay)
            for off, value in writes:
                await tb.write(off, value)

        await tb.start(0, what=f"busy writes at {delay}", during=hammer)
        after = {off: await tb.read(off) for off in RW_REGS}
        assert after == before, "a register changed while BUSY"


@cocotb.test()
async def test_int8_byte_enables(dut):
    """Partial INT8 words leave neighbouring bytes of C untouched."""
    tb = await Testbench.create(dut, "byte_enables")
    for n in range(1, ARR + 1):
        for pad in (0, 4, 12):
            m, k = tb.rng.randint(1, ARR), tb.rng.randint(1, 50)
            c_stride = round4(n) + pad
            # Fresh random bytes all over the C region before each op.
            tb.mem.write(C_REGION, tb.rng.randbytes(4096))
            await tb.run_op(Op(a=tb.matrix(m, k), b=tb.matrix(n, k), c_stride=c_stride,
                               quant=quant(7, True)))
            for addr, be, _ in tb.writes:
                assert be in (0b0001, 0b0011, 0b0111, 0b1111), f"byte enables {be:04b}"


@cocotb.test()
async def test_random_regression(dut):
    """Seeded random operations, including ACC_KEEP / NO_WB sequences."""
    tb = await Testbench.create(dut, "regression")
    total_cycles = 0
    for i in range(RANDOM_OPS):
        op = tb.random_op()
        total_cycles += await tb.run_op(op)
        if i % 50 == 0:
            dut._log.info("op %d: %s", i, op.describe())
    # Flush all accumulators through a final write-back.
    await tb.run_op(Op(a=np.zeros((ARR, 1), dtype=np.int64), b=tb.matrix(ARR, 1),
                       ctrl=CTRL_ACC_KEEP))
    dut._log.info("%d random ops, %d NPU cycles", RANDOM_OPS, total_cycles)


@cocotb.test()
async def test_cycles(dut):
    """CYCLES for representative shapes; checked against the phase formula."""
    tb = await Testbench.create(dut, "cycles")
    shapes = [(8, 8, 64), (8, 8, 256), (8, 8, 1024), (1, 8, 784), (8, 8, 16), (4, 4, 128)]
    rows = []
    for m, n, k in shapes:
        m, n, k = min(m, ARR), min(n, ARR), min(k, KMAX)
        for label, kw in (("INT32", {}),
                          ("INT8+bias", {"bias": np.arange(n), "quant": quant(8, True, True)}),
                          ("NO_WB", {"ctrl": CTRL_NO_WB})):
            cycles = await tb.run_op(Op(a=tb.matrix(m, k), b=tb.matrix(n, k), **kw))
            rows.append((m, n, k, label, cycles, m * n * k / cycles))
    lines = [f"ARR={ARR} KMAX={KMAX}", "",
             "| M | N | K | output | CYCLES | MACs/cycle |", "|---|---|---|---|---|---|"]
    lines += [f"| {m} | {n} | {k} | {label} | {c} | {r:.2f} |" for m, n, k, label, c, r in rows]
    for line in lines:
        dut._log.info(line)
    report = os.environ.get("NPU_REPORT")
    if report:
        with open(report, "w") as f:
            f.write("\n".join(lines) + "\n")
