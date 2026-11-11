# Pulse SoC: design specification

This document is the contract between the parts of the project: the CPU core,
the NPU, the SoC interconnect, the reference ISS, the firmware and the
verification flow. If the code and this document disagree, one of them is a
bug. Change the document in the same commit as the behavior.

## 1. Overview

```
                 +-------------------------------- soc_top --------------------------------+
                 |                                                                          |
   +--------+    |  +------------------+   imem port   +---------------+                    |
   |  ELF   |----+->|                  |-------------->|  IMEM 64 KiB  |                    |
   | loader |    |  |    rv32_core     |               +---------------+                    |
   +--------+    |  |  RV32IM_Zicsr    |   dmem port   +-----------+   port A  +----------+ |
                 |  |  5-stage pipe    |-------------->|           |---------->|          | |
                 |  |  BTB + BHT       |               |  address  |           |   DMEM   | |
                 |  +------------------+               |  decoder  |           |  256 KiB | |
                 |          | RVFI-style retire port   |           |           | (banked, | |
                 |          v                          |           |           |  2-port) | |
                 |    commit log (sim)                 |           |---+       |          | |
                 |                                     +-----------+   |       +----------+ |
                 |                                       |     |       |  cfg        ^ port B|
                 |                                 +-----+  +--+---+   v             |       |
                 |                                 | UART|  | EXIT |  +-------------+-----+ |
                 |                                 | LEDs|  |      |  |      npu_top      | |
                 |                                 +-----+  +------+  | 8x8 INT8 systolic | |
                 |                                                    | array + DMA       | |
                 |                                                    +-------------------+ |
                 +--------------------------------------------------------------------------+
```

- **Core:** in-order, 5-stage (IF, ID, EX, MEM, WB) RV32IM with full
  forwarding, a dynamic branch predictor, and a retirement port used for
  differential verification.
- **NPU:** an output-stationary systolic array of INT8 multiply-accumulate
  processing elements with INT32 accumulators, local operand buffers, a DMA
  engine on the second DMEM port and a fused bias / requantize / ReLU
  write-back stage. Software drives it through memory-mapped registers.
- **Memories:** Harvard. IMEM is fetch-only. DMEM is built from
  word-interleaved true dual-port RAM banks: port A gives the CPU ordinary
  32-bit access and port B gives the NPU one aligned `NPU_DMA_BYTES` block
  (16 bytes by default) per cycle, so the NPU streams operands without
  stealing CPU cycles (section 4.5).
- **Reference model:** `tools/iss` is a C++ instruction-set simulator of the
  whole SoC (core, memory map, UART, exit register and a functional model of
  the NPU). It is the golden model for the RTL.

## 2. ISA

- **RV32I** base, **M** extension, and the **Zicsr** instructions
  (`csrrw csrrs csrrc csrrwi csrrsi csrrci`).
- CSRs implemented (read-only, writes are ignored): `cycle` 0xC00, `cycleh`
  0xC80, `instret` 0xC02, `instreth` 0xC82, and their machine-mode aliases
  `mcycle` 0xB00, `mcycleh` 0xB80, `minstret` 0xB02, `minstreth` 0xB82.
  `time`/`timeh` (0xC01/0xC81) read as `cycle`/`cycleh`. Every other CSR
  reads as 0. `cycle` values are implementation-defined, so tests that compare
  the ISS with the RTL must not depend on them. `instret` counts retired
  instructions and must match between the ISS and the RTL. A counter read
  returns the value from before the reading instruction retires, so `instret`
  reads the number of instructions retired before the CSR instruction (as in
  Spike). The ISS has no clock and reports `instret` for `cycle` and `time`.
- `FENCE` and `FENCE.I` execute as no-ops.
- **No traps or interrupts.** `ECALL`, `EBREAK` and illegal instructions
  *halt* the core when they reach WB (see section 7 for exit codes).
- Misaligned loads and stores are not supported (behavior undefined; the
  compiler never emits them).

## 3. Memory map

| Base          | Size    | Region | Notes |
|---------------|---------|--------|-------|
| `0x0000_0000` | 64 KiB  | IMEM   | instruction fetch only. Reset PC = `0x0000_0000`. |
| `0x1000_0000` | 256 KiB | DMEM   | CPU loads/stores (port A, 32-bit) and NPU DMA (port B, `NPU_DMA_BYTES` wide). Stack at the top. |
| `0x2000_0000` | 4 KiB   | MMIO   | peripherals, table below. |
| `0x3000_0000` | 4 KiB   | NPU    | NPU registers, section 5. |

Decoding uses address bits `[31:28]`. Data accesses to any other address
(including IMEM) read 0 and ignore writes. Only the listed sizes are backed,
with no aliasing: addresses past the end of a region (e.g. `0x1004_0000`,
`0x2000_1000`) and undefined MMIO or NPU offsets also read 0 and ignore
writes. All MMIO and NPU registers are 32-bit and must be accessed with
`lw`/`sw`.

| Address       | Name          | Access | Meaning |
|---------------|---------------|--------|---------|
| `0x2000_0000` | `UART_TX`     | W      | transmit the low byte |
| `0x2000_0004` | `UART_STATUS` | R      | bit 0: TX ready (always 1 in simulation) |
| `0x2000_0010` | `SIM_EXIT`    | W      | end the simulation with this exit code (0 = pass) |
| `0x2000_0020` | `LEDS`        | RW     | low 8 bits drive board LEDs |

Write-only registers read 0, `UART_STATUS` reads exactly 1 and `LEDS` reads
back `{24'b0, leds}`.

The header `sw/common/soc_map.h` is the single source of these numbers for
firmware.

## 4. Bus interfaces

All logic is synchronous to one clock `clk`, with a synchronous active-low
reset `rst_n`. Every memory and register slave has **exactly one cycle of read
latency and never stalls**: a request presented in cycle *t* is captured on
the rising edge at the end of *t*, and read data is valid during cycle *t+1*.
Writes take effect at that same edge. This matches FPGA block RAM.

### 4.1 Core instruction port

```systemverilog
output logic        imem_req,    // fetch enable
output logic [31:0] imem_addr,   // byte address, word aligned
input  logic [31:0] imem_rdata   // valid the cycle after imem_req
```

When `imem_req` is low the slave holds `imem_rdata` (BRAM output register
behavior), so the core can stall IF by de-asserting `imem_req`.

### 4.2 Core data port

```systemverilog
output logic        dmem_req,
output logic        dmem_we,
output logic [31:0] dmem_addr,   // byte address of the access
output logic [3:0]  dmem_be,     // byte enables, lane = dmem_addr[1:0] onward
output logic [31:0] dmem_wdata,  // store data already shifted into its byte lanes
input  logic [31:0] dmem_rdata   // whole aligned word, valid the cycle after a read
```

The core extracts and sign/zero-extends sub-word load data itself. The SoC
decoder routes the request by `dmem_addr[31:28]` and steers `dmem_rdata` from
the slave that was selected in the previous cycle.

### 4.3 Retirement (RVFI-style) port

The core reports every retired instruction in program order, one per cycle at
most, from WB:

```systemverilog
output logic        rvfi_valid,
output logic [31:0] rvfi_pc,
output logic [31:0] rvfi_insn,
output logic [4:0]  rvfi_rd,        // 0 when the instruction writes no register
output logic [31:0] rvfi_rd_wdata,  // 0 when rvfi_rd == 0
output logic [31:0] rvfi_mem_addr,  // word-aligned address (addr & ~3) of a load/store, else 0
output logic [3:0]  rvfi_mem_wmask, // byte mask of a store, 0 for everything else
output logic [31:0] rvfi_mem_wdata, // store data in its byte lanes, other lanes 0
output logic        halted          // high once ECALL/EBREAK/illegal has retired
```

### 4.4 NPU module

```systemverilog
module npu_top #(
  parameter int ARR       = 8,     // array is ARR x ARR processing elements
  parameter int KMAX      = 1024,  // longest supported inner dimension
  parameter int DMA_BYTES = 4      // DMA port width in bytes: 4, 8 or 16
) (
  input  logic                   clk,
  input  logic                   rst_n,
  // register port, driven by the SoC decoder (offset within the NPU window)
  input  logic                   cfg_req,
  input  logic                   cfg_we,
  input  logic [11:0]            cfg_addr,    // byte offset, word aligned
  input  logic [31:0]            cfg_wdata,
  output logic [31:0]            cfg_rdata,   // valid the cycle after a read request
  // DMA master on DMEM port B
  output logic                   dma_req,
  output logic                   dma_we,
  output logic [31:0]            dma_addr,    // full SoC byte address, DMA_BYTES aligned
  output logic [DMA_BYTES-1:0]   dma_be,      // lane i = byte dma_addr + i
  output logic [8*DMA_BYTES-1:0] dma_wdata,
  input  logic [8*DMA_BYTES-1:0] dma_rdata,   // valid the cycle after a read request
  output logic                   irq          // high while STATUS.DONE is set (not wired to the core)
);
```

Every DMA access is one aligned block of `DMA_BYTES` bytes (little-endian:
byte `dma_addr + i` is `dma_wdata[8i +: 8]`). The register interface does not
depend on `DMA_BYTES`: operand addresses and strides only need to be 4-byte
aligned, and the NPU fetches the aligned blocks that cover each row. The
module default (4) matches an unbanked 32-bit DMEM port.

### 4.5 DMEM ports

`soc_top` has `parameter int NPU_DMA_BYTES = 16` (legal: 4, 8, 16) and
instantiates `npu_top` with `DMA_BYTES = NPU_DMA_BYTES`. DMEM is built from
`BANKS = NPU_DMA_BYTES / 4` word-interleaved banks, each a true dual-port
32-bit RAM with byte enables: DMEM word `w` (byte offset `4w` from
`0x1000_0000`) is held by bank `w mod BANKS` at row `w / BANKS`.

- **Port A (CPU, 32 bits)** decodes as in section 4.2. The bank is selected
  by byte-address bits `[3:2]` for 16 bytes (`[2]` for 8; a single bank for
  4) and only that bank is enabled; the bank number is registered with the
  request and steers the read data in the next cycle. Banking is invisible
  to software.
- **Port B (NPU, `8 * NPU_DMA_BYTES` bits)** maps `dma_addr` to block
  `(dma_addr - 0x1000_0000) / NPU_DMA_BYTES`, which is the same row of every
  bank: all banks are read or written in parallel, word `i` of the block is
  bank `i`, and `dma_be` lane `i` enables byte `dma_addr + i`. DMA accesses
  outside DMEM read 0 and drop writes.

Both ports keep one cycle of read latency and never stall. The same total
256 KiB is stored for any `NPU_DMA_BYTES`, so the bank count does not change
the amount of block RAM.

## 5. NPU programming model

Registers (offsets from `0x3000_0000`). All reset to 0 except `QMULT` (1).
RW registers read back the full 32-bit value written, and the range checks on
`M`, `N` and `K` in 5.1 use that full value.

| Offset | Name        | Access | Meaning |
|--------|-------------|--------|---------|
| `0x00` | `CTRL`      | W      | bit 0 `START`, bit 1 `ACC_KEEP`, bit 2 `NO_WB`, bit 3 `B_KEEP`, bit 4 `A_KEEP` |
| `0x04` | `STATUS`    | R      | bit 0 `BUSY`, bit 1 `DONE` (sticky until next START), bit 2 `ERR` |
| `0x08` | `A_ADDR`    | RW     | address of `A[0][0]` (INT8, row-major, `M x K`) |
| `0x0C` | `B_ADDR`    | RW     | address of `B[0][0]` (INT8, row-major, `N x K`, i.e. B transposed) |
| `0x10` | `C_ADDR`    | RW     | address of `C[0][0]` (INT8 or INT32, row-major, `M x N`) |
| `0x14` | `M`         | RW     | rows of A and C, 1..ARR |
| `0x18` | `N`         | RW     | rows of B (= columns of C), 1..ARR |
| `0x1C` | `K`         | RW     | inner dimension, 1..KMAX |
| `0x20` | `A_STRIDE`  | RW     | bytes between rows of A |
| `0x24` | `B_STRIDE`  | RW     | bytes between rows of B |
| `0x28` | `C_STRIDE`  | RW     | bytes between rows of C |
| `0x2C` | `QUANT`     | RW     | bits [4:0] `SHIFT`, bit 8 `OUT_INT8`, bit 9 `RELU` |
| `0x30` | `BIAS_ADDR` | RW     | address of an INT32 bias vector of length N; 0 = no bias |
| `0x34` | `CYCLES`    | R      | clock cycles from START to DONE of the last operation |
| `0x38` | `INFO`      | R      | `[7:0]` ARR, `[23:8]` KMAX, `[31:24]` version (= 1) |
| `0x3C`  | `QMULT`    | RW     | `[15:0]` signed requantization multiplier |

B is stored with K contiguous (an `N x K` matrix): this is the natural layout
of a fully connected layer's weights `W[out][in]`, so `C = A * W^T` needs no
transpose. Because the array computes one `ARR x ARR` output tile per command,
software tiles larger matrices; `ACC_KEEP`/`NO_WB` let it split K beyond KMAX.

**Operand reuse.** With `B_KEEP` set, the NPU does not reload B if its local
buffer already holds exactly the rows this operation would load: the last
successful operation that loaded B used the same `B_ADDR`, `B_STRIDE`, `N`
and `K`, and no `START` has been rejected with `ERR` since (nor has the NPU
been reset). Otherwise B is loaded as usual. `A_KEEP` does the same for A
with `A_ADDR`, `A_STRIDE`, `M` and `K`. The flags only change timing: the
result is that of the same operation without them, provided the memory of a
kept operand has not changed since it was loaded. Software must not modify
it in between, and must not let an operation's C overlap it. Typical use is a
tiled GEMM that keeps the weight tile (B) while it walks the rows of A. The
ISS ignores both bits.

### 5.1 Semantics (golden reference)

Writing `CTRL` with `START` set while `BUSY` is clear runs one operation.
Register writes while `BUSY` is set are ignored. In pseudocode, with all
arithmetic on INT32 wrapping unless noted:

```
BUSY = 1; DONE = 0; ERR = 0
if not (1 <= M <= ARR and 1 <= N <= ARR and 1 <= K <= KMAX) or any of
   A_ADDR, B_ADDR, C_ADDR, BIAS_ADDR, A_STRIDE, B_STRIDE, C_STRIDE is not a multiple of 4:
    ERR = 1; BUSY = 0; DONE = 1; stop            # nothing is read or written

if not ACC_KEEP: acc[i][j] = 0 for all i, j < ARR

for m < M, n < N:
    acc[m][n] += sum over k < K of  int8(mem8[A_ADDR + m*A_STRIDE + k])
                                  * int8(mem8[B_ADDR + n*B_STRIDE + k])

if not NO_WB:
    for m < M, n < N:
        v = acc[m][n] + (BIAS_ADDR != 0 ? int32(mem32[BIAS_ADDR + 4n]) : 0)
        if OUT_INT8:
            p = int64(v) * int16(QMULT[15:0])                     # exact, 48 bits
            if SHIFT > 0: p = (p + (1 << (SHIFT - 1))) >> SHIFT    # arithmetic shift
            if RELU and p < 0: p = 0
            mem8[C_ADDR + m*C_STRIDE + n] = clamp(p, -128, 127)
        else:
            if RELU and v < 0: v = 0
            mem32[C_ADDR + m*C_STRIDE + 4n] = v

CYCLES = cycles elapsed; BUSY = 0; DONE = 1
```

The NPU only writes the bytes of C it produces (byte enables on partial
words). Rows of A and B, and the bias vector, are read as the aligned
`DMA_BYTES` blocks that cover them; the other bytes of those blocks are
ignored. Software must not write memory the NPU reads, or
read memory it writes, while `BUSY` is set.

The ISS executes an operation instantly at the `START` write (it never
reports `BUSY`), and reports `CYCLES` = 0.

### 5.2 Driver flow

```c
REG32(NPU_A_ADDR) = (uint32_t)a;   ...   REG32(NPU_QUANT) = q;
REG32(NPU_CTRL) = NPU_CTRL_START | flags;
while (!(REG32(NPU_STATUS) & NPU_STATUS_DONE)) { }
```

## 6. Core microarchitecture requirements

- Five stages: IF, ID, EX, MEM, WB. Branches and jumps resolve in EX.
- Register file: 32 x 32, x0 hard-wired to 0, write-before-read in the same
  cycle (or an equivalent WB to ID bypass).
- Forwarding from EX/MEM and MEM/WB into EX. A load followed by a dependent
  instruction costs exactly one bubble.
- Branch prediction, selected by parameter `BP_MODE`: `0` = static not-taken,
  `1` = BTB (direct-mapped, `BTB_ENTRIES` default 64) + 2-bit saturating
  counter BHT. A correctly predicted taken branch costs no bubble; a
  mispredict costs the EX resolution penalty (2 bubbles).
- M extension: `MUL*` may be single-cycle or pipelined; `DIV*`/`REM*` use an
  iterative divider that stalls the pipeline. RISC-V corner cases
  (divide by zero, `INT_MIN / -1`) must be exact.
- Performance counters exposed to the testbench (hierarchical or ports):
  cycles, retired instructions, load-use stalls, branch mispredicts,
  divider stall cycles.

## 7. Simulation and verification contract

### 7.1 Simulator command line

Both `build/sim/soc_sim` (Verilator RTL) and `build/iss/iss` (ISS) accept:

```
<sim> [--max-cycles N] [--commit-log FILE] [--quiet] program.elf
```

`soc_sim` also takes `--vcd FILE` (waveform) and `--stats` (prints the
performance counters). `--max-cycles` counts cycles on the RTL and retired
instructions on the ISS; the default is 100,000,000.

- The ELF loader copies every `PT_LOAD` segment into IMEM or DMEM by physical
  address and zero-fills the rest of memory.
- **stdout** carries exactly the bytes written to `UART_TX`, nothing else.
- **stderr** carries diagnostics and, unless `--quiet`, one summary line:
  `[rtl] cycles=<n> instret=<n> cpi=<x.xxx> exit=<hex>` or
  `[iss] instret=<n> exit=<hex>`. `exit=` is `0x%08x` of the value written to
  `SIM_EXIT`, or the process exit code (2, 3 or 124) for the other endings.
- The run ends after the store to `SIM_EXIT` retires. Process exit code:
  `0` if the value written is 0, otherwise `value & 0xff`, or `1` if that is 0.
- Other endings: halt on `ECALL`/`EBREAK` -> exit code `2`; illegal
  instruction -> `3`; `--max-cycles` exceeded -> `124`. Usage and ELF-load
  errors exit with `125`.

### 7.2 Commit log

With `--commit-log FILE`, each retired instruction appends one line:

```
<pc> <insn> <rd> <rd_wdata>[ M <mem_addr> <wmask> <wdata>]
```

- `pc`, `insn`, `rd_wdata`, `mem_addr`, `wdata`: 8 lowercase hex digits;
  `rd`: 2 decimal digits; `wmask`: 1 hex digit.
- `rd` is `00` and `rd_wdata` is `00000000` when no register is written
  (including writes to x0).
- The ` M ...` suffix appears only for stores; `mem_addr` is word aligned and
  `wdata` holds the store data in its byte lanes (other lanes 0).
- The `SIM_EXIT` store is the last line. A halting `ECALL`/`EBREAK`/illegal
  instruction is logged as the last line with `rd` = `00`.

Example:

```
00000010 10000537 10 10000000
00000018 00b52023 00 00000000 M 10000000 f 00000017
```

The RTL log comes from the retirement port; the ISS log from its execute
loop. `tools/difftest.py` runs a program on both and reports the first line
that differs.

## 8. Repository layout

| Path | Contents |
|------|----------|
| `rtl/core/` | `rv32_core` and its submodules; `core.f` file list (paths relative to the .f, read with Verilator `-F`) |
| `rtl/npu/` | `npu_top` and submodules; `npu.f` file list |
| `rtl/soc/` | `soc_top`, memories, decoder, UART, exit register; `soc.f` |
| `sim/` | Verilator C++ harness for `soc_top`, builds `build/sim/soc_sim` |
| `tools/iss/` | reference ISS, builds `build/iss/iss` |
| `tools/rvgen/` | constrained-random RV32IM program generator |
| `tools/difftest.py` | RTL vs ISS commit-log comparison |
| `sw/common/` | crt0, linker script, `soc_map.h`, `soc.h`, mini libc |
| `sw/tests/`, `sw/bench/` | firmware tests and benchmarks (one dir per program) |
| `sw/lib/` | NPU driver and kernels shared by benchmarks |
| `models/mnist/` | training + INT8 quantization, generated C headers |
| `verif/riscv-tests/` | official riscv-tests (submodule) + Pulse env |
| `verif/npu/` | cocotb unit tests for the NPU |
| `verif/random/` | random differential tests |
| `fpga/` | Yosys/nextpnr synthesis for the ULX3S (Lattice ECP5-85F) |

## 9. RTL conventions

- SystemVerilog-2012, restricted to what **both Verilator 5 and Yosys
  (`read_verilog -sv`)** accept: `logic`, `always_ff`, `always_comb`,
  `typedef enum logic [...]`, packages of `localparam`s, parameters. No
  interfaces, classes, `program` blocks or unpacked-struct ports.
- One module per file, file name = module name.
- Synchronous active-low reset `rst_n`. No latches, no combinational loops,
  no `initial` blocks in synthesizable RTL (except `$readmemh` memory init
  guarded by a parameter).
- `verilator --lint-only -Wall` must be clean (waivers in a `lint.vlt`, each
  with a comment saying why).
