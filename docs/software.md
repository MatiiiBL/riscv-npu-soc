# Pulse software

Firmware for the Pulse SoC: the bare-metal runtime, the NPU driver and its
software twin, the firmware tests, the benchmarks, and the INT8 MNIST model
they run. The hardware contract (memory map, NPU registers and semantics) is
[spec.md](spec.md); this document covers the software side of it.

```
sw/common/   crt0.S, link.ld, soc_map.h, soc.h, lib.c (mini libc), common.mk
sw/lib/      npu.{h,c} driver, gemm.h + sw_gemm.c reference kernels,
             bench.{h,c}, rng.h, lib.mk
sw/tests/    one directory per test: Makefile, source, expected.txt
sw/bench/    gemm/, mnist/ (model.h, testset.h generated), coremark/
models/mnist training, quantization, bit-exact INT8 reference, export
tools/collect_bench.py   runs the benchmarks, writes build/bench/results.*
```

## Toolchain

Firmware is built with LLVM: `clang --target=riscv32-unknown-elf` and `lld`.
Both ship a RISC-V backend on macOS (Homebrew `llvm` and `lld`) and on Ubuntu
(`clang lld llvm`), so no GNU cross toolchain is needed. `mk/toolchain.mk`
finds them and defines the flags every program uses:

| Flag | Why |
|------|-----|
| `-march=rv32im_zicsr -mabi=ilp32` | the core implements RV32IM and the counter CSRs, no floating point |
| `-mcmodel=medany` | DMEM is at `0x1000_0000`, beyond the reach of `medlow` |
| `-ffreestanding -fno-builtin -nostdlib` | no C library and no compiler runtime: everything comes from `sw/common/lib.c` |
| `-ffunction-sections -fdata-sections -Wl,--gc-sections` | unused functions are dropped, so linking the whole of `sw/lib` into every program costs nothing |
| `-mno-relax` | keeps the linker from rewriting `gp`-relative accesses |
| `-O2 -Wall -Wextra` | every program builds with no warnings |

Because there is no compiler runtime, code must avoid operations that call
one: 64-bit division and modulo, and floating point. 64-bit multiplication
of 32-bit operands (`(int64_t)a * b`) and 64-bit shifts are fine; clang
emits `mul`/`mulh` and inline shift sequences for them.

`make sw` builds every program into `build/sw/<name>.elf`, with a
disassembly (`.dis`) next to it. There is no flat binary: IMEM and DMEM
are 256 MiB apart in the address space, so one would be almost all
padding. Every loader reads the ELF (`tools/elf2hex.py` turns it into
block-RAM images for the FPGA).

## Memory layout and startup

| Region | Contents |
|--------|----------|
| IMEM `0x0000_0000`, 64 KiB | `.text` (`crt0` first, at the reset vector) |
| DMEM `0x1000_0000`, 256 KiB | `.rodata`, `.data`, `.bss`, then free space, then the stack at the top |

The memories are Harvard: constant data lives in DMEM, not IMEM, because the
core cannot load from IMEM. Every loader (RTL testbench, ISS, FPGA image)
places each ELF segment at its address directly, so `crt0` copies nothing:
it sets `sp` and `gp`, clears `.bss`, calls `main()` and writes its return
value to `SIM_EXIT`. A program passes by returning 0. The linker script
reserves 8 KiB of stack (`-Wl,--defsym=STACK_SIZE=...` to change) and fails
the link if data plus stack would not fit. The stack starts at the end of
DMEM unless a program sets `-Wl,--defsym=STACK_TOP=...` (16-byte aligned):
`npu_oob` does, because its cases use the last bytes of DMEM.

Footprint of every program (`make -C sw size`, which reads each ELF's
`STACK_SIZE`; every program uses the default 8 KiB):

| Program | IMEM (B) | DMEM (B) | DMEM incl. stack |
|---------|---------:|---------:|-----------------:|
| hello          |  1,856 |     417 |  3.3% |
| printf         |  3,336 |     855 |  3.5% |
| muldiv         |  4,312 |   1,048 |  3.5% |
| recursion      |  3,964 |     636 |  3.4% |
| crc32          |  3,720 |   6,768 |  5.7% |
| sort           |  4,004 |  12,504 |  7.9% |
| npu_gemm       | 14,980 |  76,556 | 32.3% |
| npu_ksplit     |  9,028 |  82,576 | 34.6% |
| npu_concurrent |  9,136 |  20,176 | 10.8% |
| npu_oob        |  3,872 |     792 |  3.4% |
| npu_overlap    |  2,900 |     624 |  3.4% |
| coremark       | 10,564 |   4,484 |  4.8% |
| gemm           |  8,300 |  99,584 | 41.1% |
| mnist          |  9,116 | 187,048 | 74.5% |

`mnist` is the largest: 102 KB of weights and 78 KB of test images.

## Runtime library

`sw/common/lib.c` provides `putchar`, `puts`, `printf`, `snprintf`,
`vsnprintf`, `memset`, `memcpy`, `memcmp`, `strlen` and `strcmp`. The
formatter supports `%d %i %u %x %X %c %s %p %%` with a `0` flag, a field
width and an ignored `l` modifier (long is 32 bits). It has no `-` flag, no
precision, no 64-bit and no floating-point conversions; `fmt_ratio()` in
`sw/lib/bench.h` prints fixed-point ratios instead. `soc.h` adds
`uart_putc`, `sim_exit`, `rdcycle`, `rdinstret` and `REG32()` for MMIO.

## NPU driver

`sw/lib/npu.h`. The NPU computes `C = writeback(A x B^T + bias)` for INT8
`A` (M x K) and `B` (N x K, i.e. stored transposed, the natural layout of a
layer's weights `W[out][in]`). One operation is described by a `gemm_op_t`:

```c
typedef struct {
    const int8_t  *a, *b;     /* A: M x K, B: N x K, both row-major           */
    void          *c;         /* M x N: int8_t if OUT_INT8, else int32_t      */
    const int32_t *bias;      /* N values or NULL                             */
    uint32_t m, n, k;
    uint32_t a_stride, b_stride, c_stride;   /* bytes between rows          */
    uint32_t quant;           /* NPU_QUANT: SHIFT | OUT_INT8 | RELU           */
    int16_t  qmult;           /* requantization multiplier                   */
} gemm_op_t;
```

| Function | Does |
|----------|------|
| `npu_info()` | ARR, KMAX and version from the `INFO` register |
| `npu_gemm_tile(op, ctrl)` | exactly one NPU command (M, N <= ARR, K <= KMAX); `ctrl` may add `NPU_CTRL_ACC_KEEP`, `NPU_CTRL_NO_WB`, `NPU_CTRL_A_KEEP` and `NPU_CTRL_B_KEEP` |
| `npu_gemm(op)` | any M, N >= 0 and K >= 1, tiled as described below |
| `npu_linear(layer, x, x_stride, y, y_stride, batch)` | a fully connected layer on a batch of input rows; `linear_t` holds weights, bias, sizes and write-back mode |
| `npu_commands()`, `npu_busy_cycles()` | commands started (`START` writes) and the sum of the NPU's own `CYCLES` over them, since `npu_counters_reset()` |

All calls return `NPU_OK` or `NPU_ERROR` (the NPU set `STATUS.ERR`);
`npu_gemm()` and `npu_linear()` can also return `NPU_UNSUPPORTED` without
issuing a command, for the two GEMMs the driver cannot tile (INT8 output
wider than one tile on an array narrower than 4, and K > KMAX with
KMAX < 4; see below). Neither occurs for ARR >= 4 and KMAX >= 4. All calls
are synchronous: they program the registers, write `CTRL.START`, poll
`STATUS.DONE` and return with the NPU idle. A `fence` with a compiler
memory clobber on each side of the command orders the CPU's operand stores
before the NPU's DMA and the NPU's results before the CPU's next loads; it
is free on Pulse but keeps the code correct under the RISC-V memory model.

Every address and stride given to the NPU must be a multiple of 4; the NPU
refuses anything else without touching memory. In practice: declare
operand buffers `__attribute__((aligned(4)))` and pad row lengths to a
multiple of 4 bytes (an INT8 output row of 10 values gets a stride of 12).

Example, one INT8 layer with ReLU:

```c
static const linear_t fc = {
    .w = &weights[0][0], .bias = bias, .in = 784, .out = 128,
    .quant = NPU_QUANT_OUT_INT8 | NPU_QUANT_RELU | NPU_QUANT_SHIFT(25),
    .qmult = 16464,
};
npu_linear(&fc, images, 784, hidden, 128, 8);   /* 8 images at once */
```

### Tiling strategy

The array holds one tile of ARR x ARR INT32 accumulators, so `npu_gemm()`
walks the output tile by tile and finishes each tile before starting the
next. With the inner loop over N (the case when N has at least as many
tiles as M; otherwise M and N swap roles and `B_KEEP` is used):

```
for m0 in 0, ARR, 2 ARR ... < M:          M register = min(ARR, M - m0)
  for n0 in 0, NS, 2 NS ... < N:          N register = min(NS, N - n0)
    C_ADDR = &C[m0][n0], BIAS_ADDR = &bias[n0]
    for k0 in 0, KC, 2 KC ... < K:
      A_ADDR = &A[m0][k0], B_ADDR = &B[n0][k0], K = min(KC, K - k0)
      CTRL = START | (k0 > 0 ? ACC_KEEP : 0) | (last chunk ? 0 : NO_WB)
                   | (first command of the call ? 0 : A_KEEP)

NS = ARR, except for INT8 output with N > ARR: ARR rounded down to a multiple of 4
KC = K if K <= KMAX, else KMAX rounded down to a multiple of 4
```

- **Operand loading dominates** an NPU command: the DMA brings in
  `(m + n) * ceil(K / D)` aligned blocks of `D = NPU_DMA_BYTES` bytes (16 in
  the default SoC, one block per cycle) before the array starts, against
  `K + m + n` cycles of compute (docs/npu.md, section 6), so for a full tile
  at `D = 16` loading takes about as long as computing. The NPU can
  skip reloading A or B when a command asks for exactly the rows it loaded
  last time (`A_KEEP` / `B_KEEP`; it compares address, stride, row count
  and K, so any difference simply reloads). The driver holds the operand of
  the outer loop fixed and runs the inner loop over the dimension with more
  tiles, which minimizes loads: for a batch of 8 images through a 128-wide
  layer, the images are loaded once instead of 16 times. The first command
  of a call never asks to keep, because the caller may have rewritten the
  buffers since the previous call; and C must not overlap A or B.
- **K beyond KMAX** is split into chunks that accumulate in the array:
  every chunk but the first keeps the previous sums (`ACC_KEEP`), every
  chunk but the last skips write-back (`NO_WB`). Bias, requantization and
  ReLU happen once, in the last chunk's write-back. Chunks are a multiple of
  4 long so that each chunk of an A or B row starts on a word boundary. The
  operand addresses change with every chunk, so a split GEMM reloads both
  operands for every command.
- **INT8 column tiles are a multiple of 4 wide.** `C_ADDR` must be a
  multiple of 4, and the tile at column `n0` of an INT8 C starts `n0` bytes
  into the row, so when C is wider than one tile the driver steps N by ARR
  rounded down to a multiple of 4 (6 x 4 tiles on a 6 x 6 array, two
  columns of PEs idle). On an array narrower than 4 that step is 0, and
  `npu_gemm()` returns `NPU_UNSUPPORTED`. INT32 output and an INT8 C at
  most ARR wide tile normally on any array.
- **Registers are written once per GEMM** where possible: strides, `QUANT`
  and `QMULT` are the same for every tile, so between commands the driver
  rewrites only the operand addresses and, at the edges, M, N and K.
- **ARR and KMAX come from `INFO`**, so the same binary drives a differently
  sized array; `make -C sw test` runs the NPU tests on the ISS for every
  ARR from 2 to 7 as well as the default 8.
- A GEMM takes `ceil(M/ARR) * ceil(N/NS) * ceil(K/KC)` commands, each with a
  fixed cost (register writes, polling, the write-back pipeline), so small
  K and narrow tiles amortize poorly; the GEMM benchmark shows where this
  crosses over.
- The MNIST benchmark feeds images in batches of ARR so that layer 1's
  output tiles are full: one batch is an `8 x 784` by `128 x 784` GEMM, 16
  commands.

## Software reference kernels

`sw/lib/gemm.h`: `sw_gemm(op)` and `sw_linear(...)` take the same
descriptors as the NPU calls and produce bit-identical results, including
INT32 wraparound (accumulation is done in `uint32_t`, so overflow wraps as in
the hardware instead of being undefined) and the rounding, ReLU and
clamping of `sw_writeback()`, a direct transcription of the write-back
pseudocode in spec.md section 5.1.

They are also the baseline for every speedup, so they are written to be
fast, not just correct. The output is computed in 2 x 2 register blocks
(each loaded byte feeds two MACs) with the K loop unrolled 4x. Of the
variants tried with clang -O2 (1x4, 2x2, 2x4 and 4x2 blocks, unrolled 1, 2
or 4 times) this one has the shortest inner loop, about 3.4 instructions per
MAC; larger blocks run out of registers and spill. Measured end to end, the
whole GEMM executes 3.4 to 4.2 instructions per MAC depending on shape.

## Quantization

The MNIST MLP is quantized symmetrically per layer (details and accuracy in
[models/mnist/README.md](../models/mnist/README.md)):

- input pixels map to 0..127 (`round(p * 127 / 255)`), weights to -127..127
  with scale `max|W| / 127`, biases to INT32 at the accumulator's scale;
- the hidden layer's accumulator is rescaled to INT8 by
  `M = s_x * s_w1 / s_h`, represented as `QMULT / 2^SHIFT` with a signed
  16-bit QMULT and the largest SHIFT that fits (`16464 / 2^25`), which is
  exactly what the NPU's `OUT_INT8` write-back computes;
- the output layer keeps INT32 logits (no rescale), and the prediction is
  the first index of the largest logit.

`models/mnist/int8_ref.py` runs the same integer pipeline in numpy and is
the golden model: `testset.h` carries its prediction for each exported
image, and the MNIST benchmark fails unless both the software path and the
NPU path reproduce every one.

## Tests

`make -C sw test SIMS="build/iss/iss build/sim/soc_sim"` runs every program
in `sw/tests/` on each simulator (both by default). A test passes when it
exits with 0 and its UART output equals `expected.txt` byte for byte.
Tests print only deterministic text (no cycle counts), so one
`expected.txt` serves every simulator. The NPU tests (`npu_*`) adapt to ARR
and KMAX from `INFO`, so on the ISS they run again with `--npu-arr` set to
each size in `NPU_ARRS` (default `2 3 4 5 6 7`); `expected.txt` holds the
ARR = 8 output, so those runs are judged by the exit code.

| Test | Checks |
|------|--------|
| `hello` | the toolchain, crt0 and UART |
| `printf` | every supported conversion, flag and width via `snprintf` against literal strings; truncation and return values |
| `sort` | quicksort and merge sort against insertion sort on random, presorted, reversed, constant and few-valued inputs, every length 0..33 |
| `crc32` | CRC-32 and CRC-32C, bitwise and table-driven, against the published check values and each other, one-shot and chunked |
| `recursion` | naive Fibonacci vs iterative, Ackermann, Towers of Hanoi, a recursive-descent expression parser |
| `muldiv` | all 8 M instructions via inline asm against a reference with no M instructions, on all pairs of 16 corner operands, 500 random pairs, the ISA's division corner cases as literals, and back-to-back dependent chains |
| `npu_gemm` | the NPU against `sw_gemm` (pinned by literal known answers): random tiles, edge shapes, every write-back option, NO_WB / ACC_KEEP sequences, A_KEEP / B_KEEP on commands that must reload, all 13 invalid-command cases, tiled GEMMs, a linear layer; whole canary-filled output buffers are compared, so stray writes fail too. On arrays narrower than 4 it checks that `npu_gemm()` refuses INT8 output wider than one tile (`NPU_UNSUPPORTED`) without writing anything |
| `npu_ksplit` | K beyond KMAX through `npu_gemm` and by hand with uneven chunks; each line prints the number of commands the driver started (`npu_commands()`), so `expected.txt` also pins how many commands a GEMM costs |
| `npu_oob` | NPU DMA outside DMEM reads 0 and drops writes: A in IMEM and past the end of DMEM, B and the bias straddling the end, C's second row past the end, C in the MMIO window (on `UART_TX` and `SIM_EXIT`); the out-of-range accesses sit 256 KiB above known buffers, so a DMA that wrapped the address back into DMEM is caught |
| `npu_concurrent` | CPU word, halfword and byte loads and stores in every bank, in the DMA blocks right next to A, B, the bias and C, while 8 x 8 x 1024 INT32 and INT8 commands run (started by hand, polled afterwards): C equals `sw_gemm`, the operands are unchanged and the CPU's data equals the same workload rerun with the NPU idle. Where `CYCLES` is nonzero (RTL) it also checks that the NPU was still busy after the first slice of CPU work and done by its end |
| `npu_overlap` | one command whose C overlaps what it reads or writes, with literal results: C written over the bias vector (the bias is read once, before any C write), C rows closer than a row apart (later elements win, in spec order), and C written over its own A |

## Benchmarks

`make -C sw bench` (or `make bench` from the root) builds everything and runs
`tools/collect_bench.py --sim $(SIM)` with `SIM ?= build/sim/soc_sim`,
writing `build/bench/results.md` and `results.json`. Use
`make -C sw bench SIM=build/iss/iss` for a functional run on the ISS, but
note that the ISS executes NPU commands instantly and its cycle counter is
not a timing model, so only RTL cycle counts are performance results.

| Benchmark | Measures |
|-----------|----------|
| `gemm` | `sw_gemm` vs `npu_gemm` on 8x8x64, 32x32x32, 64x64x64, 64x64x256, 16x16x2048 (K > KMAX) and 64x64x256 with the fused bias/requantize/ReLU write-back: cycles, MACs per cycle, speedup, NPU busy fraction; every result checked |
| `mnist` | the 784-128-10 MLP over 100 test images in batches of ARR, software and NPU: accuracy, cycles per inference per layer, speedup; fails unless both paths agree bit for bit and match the Python predictions |
| `coremark` | EEMBC CoreMark 1.0, 10 iterations by default (`make -C sw/bench/coremark clean all ITERATIONS=n`), reported as CoreMark/MHz |

All timings are `rdcycle` deltas around the whole call, so NPU numbers include
the driver's register writes and polling, not just the array's busy time
(reported separately as `npu_busy_cycles`).

Besides its human-readable report, each benchmark prints machine-readable
lines

```
BENCH <bench> <case> key=value key=value ...
```

which are what `collect_bench.py` tabulates. It exits nonzero if a
benchmark fails, prints no `BENCH` line or runs past `--timeout` (1800 s
by default); a benchmark that times out is killed and still listed in both
results files, marked `TIMED OUT` (`"timed_out": true` in the JSON).

**CoreMark port.** `sw/bench/coremark/coremark` is the unmodified EEMBC
repository (a submodule); `port/` holds `core_portme.{c,h}`. Ticks are
cycles; the data block is static. CoreMark's `main()` always returns 0, so
the port's `portable_fini()` (called last) checks the list, matrix and state
CRCs against the 2K performance-run reference values and sets the exit code.
CoreMark also prints `ERROR! Must execute for at least 10 secs` and `Errors
detected`: its reporting rules require a 10-second run so that a coarse
timer cannot distort a published score. A simulation lasts milliseconds of
simulated time and counts cycles exactly, so the rule does not apply; the
CRC check is what decides pass or fail, and the score is an unofficial
CoreMark/MHz.

## Adding a program

1. Create `sw/tests/<name>/` (or `sw/bench/<name>/`) with `<name>.c` and a
   Makefile; the directory name must equal `PROG`:

   ```make
   PROG := mytest
   SRCS := mytest.c
   include ../../lib/lib.mk       # NPU driver, sw kernels, helpers (optional)
   include ../../common/common.mk
   ```

2. Return 0 from `main()` only when every check passed.
3. For a test, print a short deterministic report (no cycle counts) and
   commit the exact output as `expected.txt`, generated on the ISS and
   reviewed by hand:
   `build/iss/iss --quiet build/sw/mytest.elf > sw/tests/mytest/expected.txt`.
4. For a benchmark, add `BENCH` lines and its name to `BENCHMARKS` in
   `tools/collect_bench.py`.

`sw/Makefile` discovers the new directory automatically.
