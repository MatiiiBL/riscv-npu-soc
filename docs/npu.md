# Pulse NPU: design notes

`npu_top` is the neural processing unit of the Pulse SoC: an `ARR x ARR`
output-stationary systolic array of INT8 multiply-accumulate units with INT32
accumulators, local operand buffers filled by a DMA engine on DMEM port B, and
a fused bias / requantize / ReLU write-back stage. One command computes one
output tile, `m < M <= ARR`, `n < N <= ARR`, `k < K <= KMAX`:

```
acc[m][n] = (ACC_KEEP ? acc[m][n] : 0) + sum_k A[m][k] * B[n][k]
C[m][n]   = requantize_relu(acc[m][n] + bias[n])            unless NO_WB
```

The programming model (registers, semantics, driver flow) is
[spec.md section 5](spec.md#5-npu-programming-model); the port list is
section 4.4. This document describes how the RTL implements it and how fast
it is. Defaults: `ARR = 8`, `KMAX = 1024`.

| File | Contents |
|------|----------|
| `rtl/npu/npu_top.sv` | register block, parameter check, phase sequencer, DMA port mux |
| `rtl/npu/npu_dma.sv` | operand loader (A rows, B rows, bias) |
| `rtl/npu/npu_opbuf.sv` | per-row operand buffer banks and the skewing feeder (used for A and B) |
| `rtl/npu/npu_sdp_ram.sv` | simple dual-port RAM template (block RAM / LUT RAM) |
| `rtl/npu/npu_array.sv`, `npu_pe.sv` | the systolic array and its processing element |
| `rtl/npu/npu_wb.sv` | write-back pipeline: bias, requantization, ReLU, INT8 packing |
| `verif/npu/` | cocotb testbench, golden model, runner |

## 1. Block diagram

```
  cfg port                                               DMA port (DMEM port B)
     |                                                      ^              |
     v                                                      |              | dma_rdata
 +---------------------+  start  +---------+   reads        |              |
 | registers, FSM,     |-------->| npu_dma |----------------+              |
 | parameter check,    |         +---------+                               |
 | CYCLES              |              | fill tag (bank, word)              |
 +---------------------+              v                                    |
     | run, step t        +--------------------+--------------------+      |
     +------------------->| npu_opbuf A        | npu_opbuf B        |<-----+
     |                    | M rows of A        | N rows of B        |      |
     |                    +--------------------+--------------------+      |
     |                         | a_feed               | b_feed             |
     |                         v                      v                    |
     |  clr               +-------------------------------------+  +----------+
     +------------------->| npu_array: ARR x ARR PEs            |  | bias RAM |
                          +-------------------------------------+  +----------+
                                    | acc (ARR*ARR x INT32)             |
                                    v                                   v
                          +------------------------------------------------+  writes
                          | npu_wb: select, + bias, requantize, ReLU, pack |--------> DMA port
                          +------------------------------------------------+
```

The DMA reads (load phase) and the write-back writes never overlap in time,
so both share the one bus master port through a plain mux.

## 2. Sequencer

```
          START, params ok         last read data written        step == K+M+N, NO_WB clear
   IDLE ------------------> LOAD ------------------------> COMPUTE ----------------------------> WB
    ^  ^                                                      |                                 |
    |  |                 step == K+M+N, NO_WB set             |                                 |
    |  +------------------------------------------------------+         last write issued       |
    +-------------------------------------------------------------------------------------------+

   Every return to IDLE clears BUSY and sets DONE.
   START with bad parameters stays in IDLE and sets ERR and DONE in the same cycle: BUSY never
   rises, nothing is read or written, the accumulators are untouched and CYCLES reads 0.
```

- A `CTRL` write with `START` while idle checks the parameters of spec 5.1 on
  the full 32-bit register values (`M = 0x108` is out of range, it is not
  truncated). A bad set finishes in the same cycle with `ERR|DONE`.
- Every register write is dropped while `BUSY` is set, `CTRL` included. The
  configuration registers therefore stay frozen for the whole operation and
  the datapath reads them directly; only `ACC_KEEP` and `NO_WB` are latched at
  `START`.
- `CYCLES` is cleared at `START` and counts every cycle `BUSY` is high, so it
  is the exact number of cycles from the `START` write to `DONE`. Read while
  busy it returns the running count. A rejected operation leaves it at 0.
- `irq` is `STATUS.DONE`.

Register details shared with the ISS: every RW register stores and reads back
all 32 bits written (`QUANT` and `QMULT` included, although only the
documented fields are used), `CTRL` reads 0, and undefined offsets in the
4 KiB window read 0 and ignore writes.

## 3. Load phase: DMA and buffer layout

`npu_dma` walks three segments and issues one 32-bit read per cycle. The port
never stalls, so a request goes out every cycle of the phase:

| Segment | Rows | Words per row | Row address |
|---------|------|---------------|-------------|
| A | M | `ceil(K/4)` | `A_ADDR + m * A_STRIDE` |
| B | N | `ceil(K/4)` | `B_ADDR + n * B_STRIDE` |
| bias | N | 1 | `BIAS_ADDR + 4n` |

Bias words are fetched only when they will be used (`BIAS_ADDR != 0` and
`NO_WB` clear). Each request carries a tag (segment, row, word) that is
delayed one cycle to meet its read data, which is then written into the
selected bank. Row addresses are accumulated (`base += stride`), so there is
no multiplier, and wrap modulo 2^32 like the spec arithmetic.

**Operand buffers.** `npu_opbuf` holds one bank per array row (A side) or
column (B side). Bank `i` stores row `i` of the operand as little-endian
words:

```
bank i, word w  =  { X[i][4w+3], X[i][4w+2], X[i][4w+1], X[i][4w] }      depth ceil(KMAX/4)
```

A bank is 32 bits wide rather than 8 so the DMA can write a whole bus word
per cycle; each bank still has its own read address, so it can deliver a
different `k` than its neighbours (section 4). A bank is a plain simple
dual-port RAM: at `KMAX = 1024` it is 256 x 32 and maps to one ECP5 `DP16KD`
(16 for A and B at `ARR = 8`). The bias vector is an `ARR x 32` RAM that maps
to LUT RAM. Bytes past `K` in the last word of a row are loaded but never fed
to the array.

## 4. Compute phase: array and skew

Each processing element (`npu_pe`) is

```
          b_in
           |
           v
 a_in -->[ a_q ]--> a_out          prod_q <= a_in * b_in          (INT8 x INT8 -> INT16, a DSP)
          [ b_q ]--> b_out (down)   acc    <= (clr ? 0 : acc) + prod_q   (INT32, wrapping)
```

Operands move one PE per cycle: A to the right, B down. The array is
output-stationary: `acc` of PE `(i, j)` is `C[i][j]` and never moves.

**Skew.** For PE `(i, j)` to multiply matching operands, `A[i][k]` must enter
row `i` at cycle `k + i` and `B[j][k]` must enter column `j` at cycle `k + j`;
both then reach PE `(i, j)` at cycle `k + i + j`. The feeders produce this by
giving bank `i` the read address `k = t - i` at compute step `t`, so the skew
costs one subtractor per bank instead of a triangle of delay registers. For
`ARR = 3` the edges look like this (`.` is a zero):

```
 edge cycle:    0    1    2    3    4    5    6
 a_feed[0]    A00  A01  A02  A03  ...
 a_feed[1]     .   A10  A11  A12  A13  ...
 a_feed[2]     .    .   A20  A21  A22  A23  ...      PE(i,j) at cycle k + i + j:
 b_feed[0]    B00  B01  B02  B03  ...                  A[i][k] * B[j][k]
 b_feed[1]     .   B10  B11  B12  B13  ...
 b_feed[2]     .    .   B20  B21  B22  B23  ...
```

A feeder outputs zero when `k` is outside `0..K-1` or its row/column is not
part of the operation (`i >= M` or `j >= N`). Idle PEs therefore multiply by
zero, the array needs no valid bits, and accumulators outside the `M x N`
tile keep their values, as `ACC_KEEP` requires.

**Pipeline and drain.** A feed value is the bank address at step `t`, the RAM
word at `t + 1` and a registered byte at `t + 2`; the PE registers the
product one cycle after it sees its operands and accumulates the next. The
last product, for `(K-1, M-1, N-1)`, is accumulated at the end of step
`K + M + N`, so the phase is `K + M + N + 1` cycles. Without `ACC_KEEP` the
accumulators are cleared in step 0 (`clr` starts a new sum rather than
zeroing, so the first product is never lost).

## 5. Write-back phase: requantization

`npu_wb` drains the tile in row-major order, one element per cycle:

| Stage | Work |
|-------|------|
| S0 | issue `(m, n)`; bias RAM read address `n` |
| S1 | `acc[m][n]` through a 64:1 mux; bias word available |
| S2 | `v = acc + bias` (INT32, wrapping; bias 0 when `BIAS_ADDR = 0`) |
| S3 | partial products `hi = v[31:16] * q` and `lo = {0, v[15:0]} * q` (one DSP each) |
| S4 | `p = hi * 2^16 + lo + round`, exactly, in 48 bits |
| S5 | `r = p >>> SHIFT` (arithmetic) |
| S6 | ReLU, clamp or pass, INT8 packing, registered bus write |

The arithmetic is spec 5.1 bit for bit:

```
q     = int16(QMULT[15:0])                  INT32 output: q = 1, SHIFT = 0, so r == v
p     = int64(v) * q                        |p| <= 2^46: fits 48 bits with the rounding term
round = SHIFT ? 1 << (SHIFT - 1) : 0        round half up: (p + round) >> SHIFT
r     = (p + round) >>> SHIFT
INT8  : y = clamp(RELU && r < 0 ? 0 : r, -128, 127)
INT32 : y = RELU && v < 0 ? 0 : v
```

Splitting the 32 x 16 multiply into two 16-bit halves gives each DSP block a
registered 17 x 16 product and moves the 48-bit sum into its own stage. INT32
output reuses the same pipeline with unit gain, so both formats have the same
timing and share one datapath.

**INT8 packing.** Rows of C start word-aligned (`C_ADDR` and `C_STRIDE` are
multiples of 4), so output `n` of a row lands in byte lane `n mod 4`. The
packer collects lanes and writes when lane 3 is filled or the row ends, with
byte enables for exactly the lanes produced: neighbouring bytes of C are
never written. Writes leave in the same order as the spec's loop, so even
overlapping output rows (`C_STRIDE` smaller than the row) end in the same
state as the reference.

## 6. Performance

### Cycle formula

Measured `CYCLES` matches this formula exactly for every operation in the
test suite (the testbench asserts it for each one):

```
CYCLES = LOAD + COMPUTE + WB

LOAD    = (M + N) * ceil(K/4) + Nb + 1      Nb = N if BIAS_ADDR != 0 and not NO_WB, else 0
COMPUTE = K + M + N + 1
WB      = M * N + 6                         0 with NO_WB
```

- LOAD: one read per cycle, plus one cycle for the last read data to land.
- COMPUTE: the last operand pair (`k = K-1`) reaches PE `(M-1, N-1)` at step
  `(K-1) + (M-1) + (N-1) + 2 = K+M+N-1` (skew plus the two feeder stages).
  Its product is registered at the end of that step and accumulated at the
  end of the next, so the phase covers steps `0 .. K+M+N`.
- WB: one element per cycle plus the six-stage pipeline.

### Measured (ARR = 8, KMAX = 1024)

From `test_cycles` (written to `build/npu/arr8_k1024/cycles.md`). MACs/cycle
counts `M * N * K` useful multiply-accumulates; the peak is 64.

| M | N | K | output | CYCLES | load / compute / wb | MACs/cycle |
|---|---|---|--------|-------:|---------------------|-----------:|
| 8 | 8 | 64 | INT32 | 408 | 257 / 81 / 70 | 10.04 |
| 8 | 8 | 64 | INT8 + bias | 416 | 265 / 81 / 70 | 9.85 |
| 8 | 8 | 256 | INT32 | 1368 | 1025 / 273 / 70 | 11.98 |
| 8 | 8 | 1024 | INT32 | 5208 | 4097 / 1041 / 70 | 12.58 |
| 8 | 8 | 1024 | NO_WB (K split) | 5138 | 4097 / 1041 / 0 | 12.76 |
| 1 | 8 | 784 | INT32 | 2573 | 1765 / 794 / 14 | 2.44 |
| 1 | 8 | 784 | INT8 + bias | 2581 | 1773 / 794 / 14 | 2.43 |
| 8 | 8 | 16 | INT32 | 168 | 65 / 33 / 70 | 6.10 |

### The bound: 32-bit operand bandwidth

The array needs `M + N` new operand bytes per step (`2 * ARR = 16` for a full
tile) but the DMA port delivers 4 bytes per cycle, so for a full tile the
load phase takes `4x` as long as the compute phase. For large `K` the
throughput tends to `ARR^2 / (ARR/2 + 1)`: 12.8 MACs/cycle at `ARR = 8`
(20% of peak) and 5.3 at `ARR = 4`. The `(1, 8, 784)` row is a batch-1 fully
connected layer: each weight is used once, so it is purely load-bound.

### FPGA resources

Yosys 0.70 `synth_ecp5 -top npu_top`, ECP5 cells after flattening:

| ARR | LUT4 | CCU2C (carry) | FF | DP16KD | MULT18X18D | DPR16X4 (LUT RAM) |
|----:|-----:|-----:|-----:|------:|------:|----:|
| 8 | 5960 | 1271 | 5112 | 16 | 66 | 8 |
| 4 | 2803 | 476 | 2018 | 8 | 18 | 8 |

That is about 7% of the LUTs, 8% of the block RAMs and 42% of the 18x18
multipliers of the ULX3S's LFE5U-85F at `ARR = 8`. Per block at `ARR = 8`
(hierarchical synthesis, before cross-boundary optimization):

| Block | LUT4 | CCU2C | FF | other |
|-------|-----:|-----:|-----:|-------|
| 64 x `npu_pe` | 2112 | 1024 | 4096 | 64 MULT18X18D |
| `npu_wb` | 2499 | 89 | 395 | 2 MULT18X18D; about 1850 LUT4 of it is the 64:1 x 32-bit accumulator mux |
| 2 x `npu_opbuf` | 492 | 158 | 168 | 16 DP16KD |
| `npu_dma` | 428 | 40 | 93 | |
| `npu_top` (registers, sequencer) | 576 | 67 | 468 | bias RAM: 8 DPR16X4, 32 FF |

Every pipeline stage is register to register through at most one of: an
8 x 8 or 17 x 16 DSP multiply, a 32-bit carry chain, the 64:1 mux, a 48-bit
three-input add or the 48-bit barrel shift. Fmax has not been measured: there is no place-and-route in this
flow yet (`fpga/` will add nextpnr).

## 7. Verification

`make -C verif/npu` (or `make test-npu`) builds `npu_top` with Verilator for
`ARR = 8` and `ARR = 4` and runs the cocotb suite on each; it exits nonzero if
any test fails. Options: `ARRS=8`, `SEED=n`, `OPS=n`, `TEST=test_name`,
`make waves`.

- `npu_model.py` is the golden model, written straight from the spec 5.1
  pseudocode (numpy for the dot products, Python integers for the
  requantization). It keeps the accumulators between operations, so
  `ACC_KEEP` / `NO_WB` sequences are checked too.
- The DMEM model sits on the DMA port with one cycle of read latency, byte
  enables and full SoC addresses. In every cycle without a read response it
  drives random data, so using the bus data at the wrong time fails.
- For every operation the testbench checks the whole 256 KiB DMEM image
  against the model, `STATUS` right after `START` and at the end, that `irq`
  agrees with `DONE`, `CYCLES` against the formula above, and the exact
  sequence of DMA read addresses.
- Verilator runs with `--x-initial unique` and randomized initial values, so
  nothing depends on registers or RAM starting at zero.

| Test | Covers |
|------|--------|
| `test_reset_values` | reset values, `INFO`, undefined offsets |
| `test_register_readback` | 32-bit read-back, read-only and undefined writes dropped, `CTRL` reads 0 |
| `test_known_values` | a hand-computed product |
| `test_shapes` | every `(M, N)`; `K` = 1..9, 13, 64, 255, KMAX-3..KMAX; INT32 and INT8; the largest possible sums |
| `test_relu_bias` | bias (small and full-range, wrapping) and ReLU in both formats |
| `test_requant_edges` | rounding ties, `SHIFT` 0/1/2/7/15/16/30/31, `QMULT` 1, -1, 32767, -32768, ...; saturation at both ends; `QMULT[31:16]` ignored |
| `test_requant_random` | random products with negative and large `QMULT` |
| `test_strides` | strides larger than the row, zero strides, overlapping output rows |
| `test_k_split` | `NO_WB`, `ACC_KEEP\|NO_WB`, `ACC_KEEP` over `K > KMAX`, against an independent reference |
| `test_acc_keep_partial` | `ACC_KEEP` on smaller tiles leaves the other accumulators alone |
| `test_errors` | every invalid parameter: `ERR\|DONE`, no memory traffic, accumulators kept |
| `test_busy_writes` | writes (`START` included) during LOAD, COMPUTE and WB are ignored |
| `test_int8_byte_enables` | partial INT8 words for every `N`, padded strides |
| `test_random_regression` | 300 seeded random operations mixing all of the above |
| `test_cycles` | the measurements in section 6 |

The suite has also been run for `ARR` = 1, 2, 3, 5, 16 with `KMAX` = 16, 8,
64, 37, 64 and the random regression for 1500 operations on several seeds. As
a check on the checks, ten single-line bugs were injected into the RTL
(rounding constant, missing row gating, a drain one cycle short, full byte
enables, writes accepted while busy, a saturation threshold, no accumulator
clear, bias always fetched, ReLU ignored for INT32, `ACC_KEEP` ignored); the
suite fails on every one of them.

## 8. Limits and next steps

- **Operand bandwidth** (section 6) is the main limit: 80% of a large
  operation is the load phase. The cheapest fixes, in order:
  1. *Wider DMA port.* DMEM is block RAM, so port B can be 64 or 128 bits wide
     at no memory cost. At 128 bits the load phase of an 8 x 8 x 1024 tile
     drops from 4097 to about 1025 cycles, and the whole operation runs 2.4x
     faster.
  2. *Overlap load with compute.* Each DP16KD is only half used at
     `KMAX = 1024`, so the banks can be double-buffered for free: the DMA fills
     one half for the next command while the array computes from the other.
     This needs a queued (shadow) register set.
  3. *Operand reuse.* When software tiles a GEMM, consecutive commands often
     share B (the weights). A "keep B" control bit would skip reloading it and
     halve the load for full tiles.
- **Load, compute and write-back are sequential.** A command cannot start
  until the previous write-back is done. Overlapping them needs a second set
  of accumulators or a drain register per PE.
- **The accumulator read-out mux** is the largest single block (about 30% of
  the LUTs at `ARR = 8`) and grows as `ARR^2`. It is kept because it reads
  the accumulators without disturbing them, which `ACC_KEEP` after a
  write-back needs. Shifting the results out through the array instead puts
  a multiplexer in front of every accumulator and has to rotate the values
  back into place, so it is not clearly cheaper.
- **One output tile per command.** `M, N <= ARR`; software tiles larger
  matrices and splits `K > KMAX` with `ACC_KEEP` / `NO_WB`.
- **Fmax** is unmeasured until the FPGA flow runs place-and-route. The first
  stages to pipeline further if needed are the accumulator mux (S1) and the
  START decode from the register port.
