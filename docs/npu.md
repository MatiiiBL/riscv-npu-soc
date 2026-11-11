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
it is. Parameters: `ARR = 8`, `KMAX = 1024`, and `DMA_BYTES = 4` (the DMA
port width: 4, 8 or 16 bytes).

| File | Contents |
|------|----------|
| `rtl/npu/npu_top.sv` | register block, parameter check, operand-reuse tags, phase sequencer, bias registers, DMA port mux |
| `rtl/npu/npu_dma.sv` | operand loader: aligned blocks covering the rows of A, B and the bias vector |
| `rtl/npu/npu_opbuf.sv` | operand buffer (lane RAMs shared by groups of four rows) and the skewing feeder, used for A and B |
| `rtl/npu/npu_sdp_ram.sv` | simple dual-port RAM template (block RAM / LUT RAM) |
| `rtl/npu/npu_array.sv`, `npu_pe.sv` | the systolic array and its processing element |
| `rtl/npu/npu_wb.sv` | write-back pipeline: bias, requantization, ReLU, INT8 packing |
| `verif/npu/` | cocotb testbench, golden model, runner |

## 1. Block diagram

```
  cfg port                                               DMA port (DMEM port B, DMA_BYTES wide)
     |                                                      ^              |
     v                                                      |              | dma_rdata
 +---------------------+  start  +---------+   block reads  |              |
 | registers, FSM,     |-------->| npu_dma |----------------+              |
 | parameter check,    |         +---------+                               |
 | A/B reuse tags,     |              | fill: row, LANES x (index, word)   |
 | CYCLES              |              v                                    |
 +---------------------+  +--------------------+--------------------+      |
     | run, step t        | npu_opbuf A        | npu_opbuf B        |<-----+
     +------------------->| M rows of A        | N rows of B        |      |
     |                    +--------------------+--------------------+      |
     |                         | a_feed               | b_feed             |
     |                         v                      v                    |
     |  clr               +-------------------------------------+  +----------------+
     +------------------->| npu_array: ARR x ARR PEs            |  | bias registers |
                          +-------------------------------------+  +----------------+
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
          START, params ok         last block written            step == K+M+N+1, NO_WB clear
   IDLE ------------------> LOAD ------------------------> COMPUTE ----------------------------> WB
    ^  ^                                                      |                                 |
    |  |                 step == K+M+N+1, NO_WB set           |                                 |
    |  +------------------------------------------------------+         last write issued       |
    +-------------------------------------------------------------------------------------------+

   Every return to IDLE clears BUSY and sets DONE.
   START with bad parameters stays in IDLE and sets ERR and DONE in the same cycle: BUSY never
   rises, nothing is read or written, the accumulators are untouched and CYCLES reads 0.
```

- A `CTRL` write with `START` while idle checks the parameters of spec 5.1 on
  the full 32-bit register values (`M = 0x108` is out of range, it is not
  truncated). A bad set finishes in the same cycle with `ERR|DONE`.
- In the same cycle the reuse check (section 3.3) decides whether A and B
  are loaded.
- Every register write is dropped while `BUSY` is set, `CTRL` included. The
  configuration registers therefore stay frozen for the whole operation and
  the datapath reads them directly; only the `CTRL` flags are latched at
  `START`.
- `CYCLES` is cleared at `START` and counts every cycle `BUSY` is high, so it
  is the exact number of cycles from the `START` write to `DONE`. Read while
  busy it returns the running count. A rejected operation leaves it at 0.
- `irq` is `STATUS.DONE`.

Register details shared with the ISS: every RW register stores and reads back
all 32 bits written (`QUANT` and `QMULT` included, although only the
documented fields are used), `CTRL` reads 0, and undefined offsets in the
4 KiB window read 0 and ignore writes.

## 3. Load phase

### 3.1 DMA

`npu_dma` walks up to three segments and issues one read of an aligned
`DMA_BYTES` block per cycle. The port never stalls, so a request goes out
every cycle of the phase:

| Segment | Rows | Bytes per row | Row address | Loaded when |
|---------|------|---------------|-------------|-------------|
| A | M | K | `A_ADDR + m * A_STRIDE` | not kept (3.3) |
| B | N | K | `B_ADDR + n * B_STRIDE` | not kept (3.3) |
| bias | 1 | 4N | `BIAS_ADDR` | `BIAS_ADDR != 0` and `NO_WB` clear |

Addresses and strides only need 4-byte alignment, whatever `DMA_BYTES` is, so
a row generally starts in the middle of a block. The DMA fetches the aligned
blocks that cover the row, `ceil(((row_addr mod DMA_BYTES) + bytes) / DMA_BYTES)`
of them, and keeps a count of the bytes left to the end of the row to find
the last one. Row addresses are accumulated (`base += stride`), so there is no
multiplier, and wrap modulo 2^32 like the spec arithmetic.

Each request carries a tag (segment, row, block index `j`, and `off`, the
32-bit word of the first block where the row starts) that is delayed one
cycle to meet its data. The block is then split into `LANES = DMA_BYTES / 4`
32-bit lanes for the buffers. Row word `w` lives in lane `w mod LANES` at
index `w / LANES`, so lane `l` takes bus word `(l + off) mod LANES` of the
block, at index `j`, or `j - 1` when `l + off` wraps past the end of the
block. Lanes before the row start or past its end are not written. For
`DMA_BYTES = 16` and a row starting at word 3 of its first block (`off = 3`),
block `j = 1` holds row words 1 to 4:

```
 bus word:      0      1      2      3
 row word:      1      2      3      4
 lane RAM:      1      2      3      0         lane l takes bus word (l + 3) mod 4
 index:         0      0      0      1         j - 1 where l + off wraps, else j
```

### 3.2 Operand buffers

`npu_opbuf` holds one operand (A or B). A row is kept as 32-bit little-endian
words, word `w` holding elements `4w .. 4w+3`. Rows are grouped by four
(group `g` = rows `4g .. 4g+3`), and each group has `LANES` lane RAMs: lane
RAM `l` holds the words `w` with `w mod LANES == l` of the group's rows, at
address `{row mod 4, w / LANES}`.

- **Writes.** A block carries `LANES` consecutive words of one row, which fall
  in `LANES` different lane RAMs wherever the row starts, so a whole block is
  written in one cycle.
- **Reads.** Row `i` consumes one word every four cycles (one byte per cycle),
  and because of the skew the four rows of a group need their next word in
  four different cycles. One read port per lane RAM therefore serves four
  rows: at step `t` group `g` reads word `floor(t/4) - g` of row
  `4g + (t mod 4)`, and that row keeps it in a word register while its four
  bytes go out.

Why this organization: a 16-byte DMA block must be written into the buffer
in one cycle, and an ECP5 block RAM accepts at most 36 bits per cycle. One
128-bit-wide RAM per row would need 4 block RAMs per row, 64 at `ARR = 8`,
while DMEM and IMEM (320 KiB) already take about 160 of the 208 block RAMs
of the ECP5-85F. Sharing lane RAMs between four rows keeps the count at 16
for `DMA_BYTES = 16` and halves it to 8 for `DMA_BYTES = 4` and 8. The price
is a 32-bit word register per row and one extra compute cycle.

At `KMAX = 1024` a lane RAM holds `4 x 256 / LANES` words: two ECP5 `DP16KD`
for `LANES = 1` (1024 x 32) and one otherwise. The bias vector is kept in
`ARR x 32` registers, since up to `LANES` words of it arrive per cycle.

### 3.3 Operand reuse: A_KEEP and B_KEEP

Each buffer remembers the address, stride, row count and `K` it was last
loaded with (a tag with a valid bit). With `CTRL.B_KEEP` set, a `START`
whose `B_ADDR`, `B_STRIDE`, `N` and `K` all match the B tag skips the B
segment; `A_KEEP` does the same for A with `A_ADDR`, `A_STRIDE`, `M`, `K`.
A load records its parameters in the tag whether or not the flag was set. A
rejected `START` (`ERR`) and reset clear both tags, so the first operation
after either always loads.

The flags never change results, only the load time, provided the memory of a
kept operand is not modified between the operations (by software, or by an
operation whose C overlaps it): spec section 5 makes that the software's
responsibility. The main use is tiled GEMM: keeping the weight tile B while
walking the row tiles of A halves the load of a full tile.

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
having row `i` present element `k = t - 2 - i` at compute step `t` (the
fetch schedule of section 3.2), so the skew costs no delay registers. For
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

**Pipeline and drain.** For element `k` of row `i` (word `w = k/4`): the
group reads the lane RAM at step `4w + i`, the word register loads at
`4w + i + 1`, the byte is selected into the feed register at `k + i + 2`,
and it is on the array edge at `k + i + 3`. The PE registers the product one
cycle after it sees its operands and accumulates the next. The last product,
for `(K-1, M-1, N-1)`, is accumulated at the end of step `K + M + N + 1`, so
the phase is `K + M + N + 2` cycles. Without `ACC_KEEP` the accumulators are
cleared in step 0 (`clr` starts a new sum rather than zeroing, so the first
product is never lost).

## 5. Write-back phase: requantization

`npu_wb` drains the tile in row-major order, one element per cycle:

| Stage | Work |
|-------|------|
| S0 | issue `(m, n)` |
| S1 | `acc[m][n]` through a 64:1 mux, `bias[n]` through an 8:1 mux |
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

**Wide port.** Write-back produces one element per cycle, so it never needs
more than 32 bits per cycle: the word is replicated into every lane of the
block and the byte enables select its lane. A wider write-back would need
several requantization pipelines and only pays off once write-back dominates
(section 8).

## 6. Performance

### Cycle formula

Measured `CYCLES` matches this formula exactly for every operation in the
test suite, in every configuration (the testbench asserts it for each one):

```
CYCLES  = LOAD + COMPUTE + WB

LOAD    = sum over loaded rows of ceil(((row_addr mod D) + K) / D)      D = DMA_BYTES
          + ceil(((BIAS_ADDR mod D) + 4N) / D)   if BIAS_ADDR != 0 and not NO_WB
          + 1
COMPUTE = K + M + N + 2
WB      = M * N + 6                              0 with NO_WB
```

For aligned rows LOAD is `(M + N) * ceil(K / D) + 1` (plus the bias), and a
kept operand drops out of the sum.

- LOAD: one block per cycle, plus one cycle for the last data to land.
- COMPUTE: the last operand pair (`k = K-1`) reaches PE `(M-1, N-1)` at step
  `(K-1) + (M-1) + (N-1) + 3 = K+M+N` (skew plus the three feeder stages).
  Its product is registered at the end of that step and accumulated at the
  end of the next, so the phase covers steps `0 .. K+M+N+1`.
- WB: one element per cycle plus the six-stage pipeline.

### Measured (ARR = 8, KMAX = 1024, INT32 output)

From `test_cycles` (written to `build/npu/arr8_k1024_d<D>/cycles.md`), with
MACs/cycle in parentheses: `M * N * K` useful multiply-accumulates per cycle,
peak 64. "Before" is the previous design (32-bit port, per-row buffers, one
compute cycle less).

| M x N x K | before | D = 4 | D = 8 | D = 16 | D = 16, B kept | D = 16, A and B kept |
|-----------|-------:|------:|------:|-------:|---------------:|---------------------:|
| 8 x 8 x 64 | 408 (10.0) | 409 (10.0) | 281 (14.6) | 217 (18.9) | 185 (22.1) | 153 (26.8) |
| 8 x 8 x 256 | 1368 (12.0) | 1369 (12.0) | 857 (19.1) | 601 (27.3) | 473 (34.6) | 345 (47.5) |
| 8 x 8 x 1024 | 5208 (12.6) | 5209 (12.6) | 3161 (20.7) | 2137 (30.7) | 1625 (40.3) | 1113 (58.9) |
| 1 x 8 x 784 | 2573 (2.44) | 2574 (2.44) | 1692 (3.71) | 1251 (5.01) | 859 (7.30) | 810 (7.74) |

With `DMA_BYTES = 4`, keeping B alone gives 3161 cycles for 8 x 8 x 1024
(20.7 MACs/cycle) and 1006 for 1 x 8 x 784 (6.23). INT8 output with bias
costs the bias blocks on top (2 cycles at `D = 16`, 8 at `D = 4` for N = 8);
`NO_WB` saves the `M * N + 6` write-back cycles.

**Tiled GEMM** (`test_tiled_gemm_keep`: C = 25 x 15 from K = 300, eight
tiles of at most 8 x 8, INT8 with bias): 11267 cycles at `D = 4` without
reuse and 7892 with `B_KEEP`; 5091 and 4215 at `D = 16`.

### The bound: operand bandwidth

A full tile needs `M + N = 2 * ARR` new operand bytes per step, while the port
delivers `D` bytes per cycle. For large `K` the throughput tends to
`ARR^2 / (2 * ARR / D + 1)` with both operands loaded and
`ARR^2 / (ARR / D + 1)` with one kept:

| ARR = 8, MACs/cycle | D = 4 | D = 8 | D = 16 |
|---------------------|------:|------:|-------:|
| A and B loaded | 12.8 | 21.3 | 32.0 |
| B kept | 21.3 | 32.0 | 42.7 |
| A and B kept | 64 | 64 | 64 |

At `D = 16` a full tile's load phase (1025 cycles at K = 1024) is as long as
its compute phase (1042), so the next limit is that the two run one after
the other (section 8). The `1 x 8 x 784` row is a batch-1 fully connected
layer: its 8 weight rows are used once, so it stays load-bound unless the
weights fit and stay in the buffer.

### FPGA resources

Yosys 0.70 `synth_ecp5 -top npu_top` (other `DMA_BYTES` and `ARR` values set
with `hierarchy -chparam`), ECP5 cells after flattening. The default
configuration is what the CI synthesis job reports:

| ARR | DMA_BYTES | LUT4 | CCU2C (carry) | FF | DP16KD | MULT18X18D | DPR16X4 |
|----:|----------:|-----:|------:|-----:|------:|------:|----:|
| 8 | before (4) | 5960 | 1271 | 5112 | 16 | 66 | 8 |
| 8 | 4 (default) | 6399 | 1297 | 6025 | 8 | 66 | 0 |
| 8 | 8 | 6542 | 1334 | 6027 | 8 | 66 | 0 |
| 8 | 16 | 7001 | 1341 | 6029 | 16 | 66 | 0 |
| 4 | 16 | 3509 | 529 | 2560 | 8 | 18 | 0 |

At `ARR = 8`, `DMA_BYTES = 16` that is about 8% of the LUTs, 8% of the block
RAMs and 42% of the 18x18 multipliers of the ULX3S's LFE5U-85F. Compared with
the previous design, the row word registers (512 FF), the bias registers
(256 FF) and the reuse tags (160 FF) add about 900 FF; the wide port adds
about 600 LUT4 of lane rotation and block-boundary logic at 16 bytes. Per
block at `ARR = 8`, `DMA_BYTES = 16` (hierarchical synthesis, before
cross-boundary optimization):

| Block | LUT4 | CCU2C | FF | other |
|-------|-----:|-----:|-----:|-------|
| 64 x `npu_pe` | 2112 | 1024 | 4096 | 64 MULT18X18D |
| `npu_wb` | 2683 | 89 | 427 | 2 MULT18X18D; about 1850 LUT4 of it is the 64:1 x 32-bit accumulator mux |
| 2 x `npu_opbuf` | 876 | 170 | 652 | 16 DP16KD |
| `npu_dma` | 525 | 83 | 106 | |
| `npu_top` (registers, reuse tags, bias, sequencer) | 918 | 67 | 884 | |

Every pipeline stage is register to register through at most one of: an
8 x 8 or 17 x 16 DSP multiply, a 32-bit carry chain, the 64:1 mux, a 48-bit
three-input add, the 48-bit barrel shift, or the lane rotation feeding a
buffer RAM write port. Fmax has not been measured: there is no
place-and-route in this flow yet (`fpga/` will add nextpnr).

## 7. Verification

`make -C verif/npu` (or `make test-npu`) builds `npu_top` with Verilator in
four configurations, `ARR:DMA_BYTES` = 8:4, 8:8, 8:16 and 4:16, and runs the
cocotb suite on each; it exits nonzero if any test fails. Options:
`CONFIGS="8:16"`, `SEED=n`, `OPS=n`, `TEST=test_name`, `make waves`.

- `npu_model.py` is the golden model, written straight from the spec 5.1
  pseudocode (numpy for the dot products, Python integers for the
  requantization). It keeps the accumulators between operations, so
  `ACC_KEEP` / `NO_WB` sequences are checked too. It also applies the
  `A_KEEP` / `B_KEEP` rule, which only decides the expected reads and CYCLES.
- The DMEM model sits on the DMA port with one cycle of read latency,
  `DMA_BYTES`-wide aligned blocks, byte enables and full SoC addresses. In
  every cycle without a read response it drives random data, so using the
  bus data at the wrong time fails.
- For every operation the testbench checks the whole 256 KiB DMEM image
  against the model, `STATUS` right after `START` and at the end, that `irq`
  agrees with `DONE`, `CYCLES` against the formula above, and the exact
  sequence of DMA block addresses.
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
| `test_misaligned_rows` | A, B, C and bias starting at every 4-byte offset of a block, strides of every residue mod 16, K from 1 to KMAX |
| `test_k_split` | `NO_WB`, `ACC_KEEP\|NO_WB`, `ACC_KEEP` over `K > KMAX`, against an independent reference |
| `test_acc_keep_partial` | `ACC_KEEP` on smaller tiles leaves the other accumulators alone |
| `test_operand_reuse` | `A_KEEP` / `B_KEEP` hits; a miss for each changed address, stride, `M`, `N` and `K`, after an `ERR`, after reset, and on the first operation; tags recorded without the flags |
| `test_tiled_gemm_keep` | a tiled 25 x 15 GEMM with `B_KEEP` (one B load per column tile) against an independent reference |
| `test_errors` | every invalid parameter: `ERR\|DONE`, no memory traffic, accumulators kept |
| `test_busy_writes` | writes (`START` included) during LOAD, COMPUTE and WB are ignored |
| `test_int8_byte_enables` | partial INT8 words for every `N`, padded strides; enables limited to one 32-bit lane |
| `test_random_regression` | 300 seeded random operations mixing all of the above, including random operand reuse (30 to 41 kept loads of each operand per run) |
| `test_cycles` | the measurements in section 6 |

The suite has also been run for `ARR:DMA_BYTES` = 1:16, 2:8, 3:8, 5:4,
5:16 and 16:16 with `KMAX` = 16, 8, 64, 37, 37 and 64 (so `KMAX` is not a
multiple of the block size), and the random regression for 1500 operations
on several seeds. As a check on the checks, single-line bugs were injected
into the RTL and the suite failed on each: ten in the original datapath
(rounding constant, row gating, drain length, byte enables, busy writes,
saturation threshold, accumulator clear, bias fetch, ReLU, `ACC_KEEP`), six
in the wide port (lane index wrap, lane rotation, group skew, write lane,
last block of a row, lanes past the row end) and six in operand reuse (keep
without the flag, ignoring the stride, `N` or `K`, keeping across an `ERR`,
the wrong flag bit). One injected change survived and is benign: dropping
the guard on lanes before the start of a row's first block lets them write
a word of the same row that a later block of that row always overwrites, or
that is never read.

## 8. Limits and next steps

- **Load and compute are sequential.** With `DMA_BYTES = 16` they take about
  the same time for a full tile, so overlapping them would nearly double the
  throughput again. At `KMAX = 1024` and 16-byte blocks every lane RAM is
  half used (256 of 512 words), so the buffers can be double-buffered at no
  block RAM cost: the DMA fills one half for the next command while the
  array computes from the other. This needs a queued (shadow) register set
  and a way to tell software the next command was accepted.
- **Write-back is one element per cycle.** For short `K` with both operands
  kept, the `M * N + 6` write-back cycles are a large share (70 of 153 for
  8 x 8 x 64). Two or four requantization pipelines feeding the wide port
  would cut it, at about 2 DSPs and a few hundred LUTs each.
- **Reuse is exact-match only.** A kept operand must have the same address,
  stride, rows and `K`. Batch-1 layers (`M = 1`) load each weight once and
  stay load-bound; they gain only from the wider port.
- **The accumulator read-out mux** is the largest single block (about 26% of
  the LUTs at `ARR = 8`, `DMA_BYTES = 16`) and grows as `ARR^2`. It is kept
  because it reads the accumulators without disturbing them, which
  `ACC_KEEP` after a write-back needs. Shifting the results out through the
  array instead puts a multiplexer in front of every accumulator and has to
  rotate the values back into place, so it is not clearly cheaper.
- **One output tile per command.** `M, N <= ARR`; software tiles larger
  matrices and splits `K > KMAX` with `ACC_KEEP` / `NO_WB`.
- **Fmax** is unmeasured until the FPGA flow runs place-and-route. The first
  stages to pipeline further if needed are the accumulator mux (S1), the
  START decode with the reuse comparison, and the lane rotation in front of
  the buffer RAMs.
