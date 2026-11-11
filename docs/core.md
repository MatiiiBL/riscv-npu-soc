# Pulse CPU core and SoC: microarchitecture

This document describes how `rtl/core/` (the `rv32_core` pipeline),
`rtl/soc/` (`soc_top` and its memories and peripherals) and `sim/` (the
Verilator harness) implement the contract in [spec.md](spec.md). The spec
says *what* the hardware must do; this file explains *how* and *why*.

| File | Contents |
|------|----------|
| `rtl/core/rv32_pkg.sv` | opcodes, CSR numbers, the ALU / operand / result-select enums |
| `rtl/core/rv32_decode.sv` | combinational decoder, illegal-instruction detection |
| `rtl/core/rv32_regfile.sv` | 32 x 32 register file, write-before-read |
| `rtl/core/rv32_alu.sv` | integer ALU |
| `rtl/core/rv32_muldiv.sv` | multiplier (operands in EX, product in MEM), iterative divider |
| `rtl/core/rv32_bpred.sv` | BTB with 2-bit counters (or nothing, for `BP_MODE = 0`) |
| `rtl/core/rv32_core.sv` | the five pipeline stages, hazards, CSRs, counters, retire port |
| `rtl/soc/soc_top.sv` | core + IMEM + DMEM + decoder + MMIO + NPU |
| `rtl/soc/bram_sp.sv`, `bram_tdp.sv` | inferable single-port ROM and true dual-port RAM |
| `rtl/soc/soc_dmem.sv` | DMEM: word-interleaved `bram_tdp` banks, 32-bit CPU port, wide NPU port |
| `rtl/soc/soc_bus.sv` | address decoder for the CPU data port and the NPU DMA port |
| `rtl/soc/soc_mmio.sv`, `uart_tx.sv` | UART, SIM_EXIT, LEDS |
| `rtl/npu/` | `npu_top`, the systolic-array accelerator (see `docs/npu.md`) |

## 1. Pipeline

```
            IF                  ID                     EX                       MEM               WB
     +--------------+   +-----------------+   +-----------------------+   +--------------+   +--------------+
     | pc_f         |   | decode, skid    |   | forwarding muxes      |   | dmem request |   | load align / |
     | BTB lookup   |   | regfile read    |   | ALU, branch compare   |   | byte enables |   |  sign extend |
     | next-PC mux  |   | imm, pc + imm   |   | redirect, BTB update  |   | store lanes  |   | regfile write|
     |              |   | interlocks      |   | DIV FSM, CSRs, MUL in |   | MUL product  |   | rvfi_*       |
     +------+-------+   +--------+--------+   +-----------+-----------+   +------+-------+   +------+-------+
            |  imem_addr         ^ imem_rdata             |                      |                  |
            v                    |                        |                      |                  |
        [ IMEM BRAM ]------------+                        |                      |                  |
            ^                                             |                      |                  |
            +-------------- redirect_pc_x ----------------+                      |                  |
                                         EX/MEM result ---> EX operands <--------+                  |
                            MEM/WB register (res_w) ---> EX operands <------------------------------+
                                         WB write  -------> regfile (seen by ID in the same cycle)
```

Signals carry a stage suffix (`_f`, `_d`, `_x`, `_m`, `_w`); every pipeline
register has a `valid` bit, and a bubble is simply `valid = 0`.

**IF.** `pc_f` is presented to IMEM and, in the same cycle, to the branch
predictor. Because IMEM has one cycle of latency, its output register *is*
the IF/ID instruction register: ID decodes `imem_rdata` directly, and only
the PC and the prediction are stored in flops. IMEM reads every cycle
(`imem_req` drops only after a halt). When ID stalls, `pc_f` holds and the
instruction ID keeps is captured in a skid register (`skid_q`), because the
RAM output already shows the next fetch; ID decodes `skid_q` until the
stall ends, and the RAM output is the right instruction again afterwards.
Gating the RAM's clock enable with the stall instead (spec 4.1 allows it)
would put the whole decode and interlock in front of the RAM (section 10).

**ID.** Decode, register-file read, the interlocks (section 1.1), and the
branch/JAL target `pc + imm`, which does not depend on registers and so is
taken out of EX.

**EX.** Forwarding muxes, ALU, branch comparator, misprediction check and
redirect, BTB update, divider, CSR reads, and the multiplier's operand
registers. The result written to `rd` (ALU, link `pc + 4`, CSR or DIV) is
selected here.

**MEM.** The load/store request goes out on the data port with byte enables
and store data already shifted into its lanes (`dmem_be`, `dmem_wdata`).
The multiply runs here from the registered operands, and its product goes
into the MEM/WB result register (`res_w`).

**WB.** Load data comes back from the SoC (steered by the decoder), is
aligned and extended, and is written to the register file; the instruction
is reported on the retirement port.

### 1.1 Data hazards and forwarding

| Producer | Consumer distance | Path | Cost |
|----------|-------------------|------|------|
| ALU, link, CSR, DIV | 1 (producer in MEM) | EX/MEM `res_m` -> EX operand mux | 0 |
| ALU, link, CSR, DIV, MUL | 2 (producer in WB) | MEM/WB `res_w` -> EX operand mux | 0 |
| any | 3 (producer writing back while consumer is in ID) | register file write-before-read | 0 |
| MUL | 1 | stall ID one cycle, then MEM/WB forward | 1 bubble |
| load | 1 | stall ID two cycles, then register file write-before-read | 2 bubbles |
| load | 2 | stall ID one cycle, then register file write-before-read | 1 bubble |

The forwarding rule for each EX operand is: take the MEM-stage result if the
instruction in MEM is valid, writes a register (`rd != x0`) and `rd`
matches; otherwise the MEM/WB result register `res_w` under the same
conditions; otherwise the value read in ID. MEM has priority because it is
the younger producer. `rd_we` is computed in the decoder as "writes a
register *and* `rd != 0`", so writes to x0 never enter the bypass network at
all, and the register file forces reads of x0 to zero. Both forwards come
from pipeline registers.

Load data is not forwarded at all. It exists only in WB and arrives late
there: 5.8 ns of block RAM clock-to-out on the FPGA, about 6 ns of routing
out of the 128 RAMs of DMEM, then the bank select, the read steering and
the alignment. An earlier version forwarded it from WB into EX, which put
all of that in front of the ALU, the multiplier, the branch comparator and
the redirect and held the clock at 27 MHz (section 10). Instead, ID waits
until the load is in WB, when the register file's write-before-read hands
the value to ID directly:

```
cycle            1    2    3    4    5    6    7
lw   a1, 0(s0)   IF   ID   EX   MEM  WB
add  a2, a1, a3       IF   ID   ID   ID   EX   MEM     a1 from the register file in cycle 5
next                       IF   IF   IF   ID   EX      (bubbles in EX in cycles 4 and 5)
```

A multiply produces its result at the end of MEM (section 3), so a consumer
right behind it stalls once and then takes the product from MEM/WB. The
interlocks cover every consumer the same way: ALU operands, branch
operands, store data, store and load addresses, `jalr` bases, multiplier
and divider operands. They guarantee that the MEM-stage forward never sees
a load's data or a product and the WB forward never a load's data, so
`wb_data_w` (which carries the load data) only goes to the register file.

The decoder reports which source registers an opcode reads (`uses_rs1`,
`uses_rs2`; for example LUI, JAL and the CSR instructions read none),
derived from the opcode alone so that the check does not wait for the full
legality decode; an illegal encoding may then stall a cycle before it
halts. The registers that loads and multiplies have not produced yet are
kept as one-hot masks, built when those instructions leave ID (`ld_x_q`,
`mul_x_q`, and `ld_m_q` one stage later) and qualified with `valid_x` /
`valid_m`, so the check is a lookup of `rs1` and `rs2` in masks that come
entirely from flip-flops. Both choices shorten the path from the IMEM
output to the stall, which is the core's critical path (section 10).

A variant that keeps one bubble for ALU consumers of a load (forwarding the
block RAM data from WB to the ALU operands and the store data only, and
giving JALR its own adder) was built and measured. It saves about a third
of the extra load-use cycles on CoreMark (CPI 1.291 instead of 1.345), but
its block RAM -> ALU -> `res_m` path limits Fmax to 37.4-38.9 MHz, so it
is slower in wall-clock time than this rule at 40 MHz. Most of CoreMark's
load consumers are branches, which would wait two bubbles in either case.

### 1.2 Control hazards

Branches, JAL and JALR resolve in EX. Every instruction carries the decision
IF actually made (`pred_taken`) and the predicted target down the pipe, and
EX redirects when the outcome differs:

```
redirect = valid && (taken != pred_taken || (taken && target != predicted target))
```

The check applies to every instruction, not only to branches, so recovery
does not depend on the predictor ever being right; a predictor bug can cost
cycles but never correctness (the directed tests were run against a
deliberately broken counter to confirm this). On a redirect, IF loads the
correct PC and the two younger instructions, in ID and IF, are discarded:

```
cycle            1    2    3    4    5    6
beq  (taken)     IF   ID   EX                     mispredicted: redirect in cycle 3
wrong path 1          IF   ID   --
wrong path 2               IF   --
target                          IF   ID   EX       2 bubbles in EX (cycles 4, 5)
```

Branch and jump targets that do not depend on registers are computed in ID.
Their comparison with the predicted target is done in EX from registers, in
parallel with the longer branch comparison (in ID it would sit behind the
instruction decode on the path from IMEM), so only JALR has an adder in the
EX redirect path.

### 1.3 Halting

ECALL, EBREAK and every encoding outside RV32IM/Zicsr decode as `is_halt`
with no other effect. When such an instruction reaches EX (where it is known
to be on the correct path, since only EX can redirect), it flushes ID and IF
and sets a sticky flag that stops fetching. It then retires normally from WB
(logged with `rd = 00`), and `halted` rises the following cycle. Nothing
younger reaches MEM, so a store behind an ECALL never happens.

A fetch from outside IMEM or from a pc that is not 4-byte aligned delivers
the illegal word 0 (section 6), so a program that jumps or runs off IMEM,
or jumps to a misaligned target, halts the same way at that pc (spec 2,
"Bad fetches"). The ISS does the same, so the two logs end on the same line.

The decoder is strict about reserved encodings that a simpler decoder might
accept, because the ISS treats them as illegal too: shift-immediates with
`imm[11:5]` other than `0000000`/`0100000` (so `slli` with `shamt[5] = 1`),
R-type `funct7` other than `0000000`, `0100000` (only for SUB/SRA) and
`0000001`, branch `funct3` 010/011, load `funct3` 011/110/111, store `funct3`
other than 000-010, JALR `funct3 != 0`, SYSTEM `funct3 = 100`, and SYSTEM
`funct3 = 000` other than the exact ECALL/EBREAK words. FENCE and FENCE.I
(MISC-MEM `funct3` 000/001) are no-ops. CSR instructions are always legal,
since every CSR is read-only with writes ignored (spec 2).

## 2. Branch prediction

`BP_MODE = 0` predicts every branch not taken; the BTB is not generated.

`BP_MODE = 1` uses a direct-mapped BTB with `BTB_ENTRIES` (default 64)
entries, indexed by `pc[log2(N)+1:2]`. Each entry holds a valid bit, the
full remaining PC bits as tag, a 30-bit target and a 2-bit saturating
counter; it predicts taken on a tag hit with counter >= 2. The lookup happens
on `pc_f` in IF, so the very next fetch already goes to the target and a
correctly predicted taken branch costs no bubble.

EX writes the entry back for each resolved control-flow instruction, using
the counter and target captured at lookup time:

| Instruction | BTB hit at lookup | Action |
|-------------|-------------------|--------|
| conditional branch | yes | counter +1 if taken, -1 if not (saturating); target refreshed if taken |
| conditional branch | no, taken | allocate: counter = 2 (weakly taken), target |
| conditional branch | no, not taken | nothing (not-taken branches would only waste entries) |
| JAL / JALR | either | allocate or refresh with counter = 3 and the resolved target |

JALR is predicted to go where it went last time, which is right for
`jalr` through a fixed pointer and for a return to a call site in a loop, and
wrong for a function returning to a different caller each time. Since an
unpredicted JALR also costs the 2-bubble redirect, predicting it is never
worse. A return-address stack would be the next improvement.

Full tags make a false hit (a non-branch predicted taken) impossible; two
branches whose PCs share index bits (256 bytes apart for 64 entries) evict
each other. Updating from the copy of the counter captured in IF instead of
re-reading the table saves a read port; the only effect is that two
in-flight instances of one branch both start from the same old counter,
which delays training by one step.

| Event | Cost |
|-------|------|
| not-taken branch, predicted not taken | 0 |
| taken branch / jump, predicted taken with the right target (`BP_MODE = 1`) | 0 |
| any misprediction (direction or target), every taken branch with `BP_MODE = 0` | 2 bubbles |

`verif/core/timing.S` measures these costs with `rdcycle`, including that a
branch is predicted taken on its second execution (counter allocated at 2),
that one not-taken outcome does not flip a saturated counter, and that two
aliasing branches executed alternately never become predicted.

On FPGA all three arrays are LUT RAM, read asynchronously from `pc_f`
(`ram_style = "distributed"` on the tag and target arrays): 56 `DPR16X4`
for tags and targets, 4 for the counters. As block RAMs they would need a
registered read address, which Yosys provides by moving `pc_f` into the
RAMs; the whole next-PC selection, including the late stall and redirect,
then ended at the RAM address pins several nanoseconds of routing away
(section 10).

## 3. Multiply and divide

**MUL, MULH, MULHSU, MULHU** span EX and MEM. In EX the forwarded operands
are sign- or zero-extended to 33 bits according to the variant and
registered (with a high/low select bit); in MEM the registered operands are
multiplied with a 64-bit result: the exact 33 x 33 product always fits, so
one multiplier serves all four instructions (MUL takes bits 31:0, the others
63:32). The product goes into the MEM/WB result register `res_w`. Verilator
evaluates it with native 64-bit arithmetic, and Yosys narrows the redundant
sign bits back to a 33 x 33 multiply on four ECP5 `MULT18X18D` blocks plus
a carry-chain adder.

*Why not in EX:* the multiply and its adder take about 20 ns on the ECP5
(routing to the DSPs, 3.9 ns through them, two carry chains). In EX they
sat behind the forwarding muxes and limited the clock to 27 MHz (section
10). In MEM they start from registers, at the cost of a "mul-use"
interlock: an instruction that reads the product right away waits one
cycle, then takes it from MEM/WB. On CoreMark that costs about 30,000
cycles (1%).

**DIV, DIVU, REM, REMU** use a radix-2 restoring divider on the operand
magnitudes, one quotient bit per cycle. The instruction stays in EX
(`div_stall` holds IF, ID and EX; MEM receives bubbles) while the unit runs:

| Cycle in EX | State | Work |
|-------------|-------|------|
| 0 | IDLE | capture `|a|`, `|b|` (forwarded operands) and the result signs |
| 1-32 | BUSY | shift / trial subtract, one quotient bit per cycle |
| 33 | DONE | sign-corrected result to EX, instruction advances |

So every divide costs exactly 34 cycles in EX (33 stall cycles), and two
back-to-back divides cost 68. The corner cases come out of the algorithm
without special datapaths: dividing by zero produces an all-ones quotient and
remainder = dividend as required, with only the quotient sign fix-up
suppressed; `INT_MIN / -1` produces `INT_MIN` with remainder 0. The trial
subtraction is 33 bits wide and its top bit is the borrow, because the
invariant `remainder < divisor` bounds the shifted remainder below twice the
divisor.

## 4. CSRs and counters

All CSR instructions read the old value into `rd`; writes are ignored, so
no CSR instruction reads `rs1`. Reads happen in EX:

| CSR | Value |
|-----|-------|
| `cycle`, `mcycle`, `time` / `...h` | 64-bit cycle counter since reset |
| `instret`, `minstret` / `...h` | instructions retired before this one |
| anything else | 0 |

`instret` must match the ISS exactly. The counter increments in WB, so when
a CSR instruction is in EX, the valid instructions in MEM and WB are older
but not yet counted; the read returns `instret + valid_m + valid_w`.
Squashed wrong-path instructions are never valid and never counted.

Performance counters are outputs of `rv32_core` (and of `soc_top`):

| Port | Counts |
|------|--------|
| `perf_cycles` | cycles since reset (64-bit) |
| `perf_instret` | retired instructions (64-bit) |
| `perf_load_use` | load-use bubbles (mul-use bubbles are not counted) |
| `perf_mispredict` | EX redirects |
| `perf_div_stall` | cycles EX was held by the divider |
| `perf_ctrl` | branches and jumps resolved |

## 5. Retirement port

`rvfi_*` (spec 4.3) are driven straight from the MEM/WB register, plus the
load data aligned in WB. One instruction retires per cycle at most, in
program order, and only valid instructions are reported, so a squashed
instruction never appears. `rvfi_rd` and `rvfi_rd_wdata` are zero when no
register is written (including writes to x0). `rvfi_mem_addr` is the
word-aligned address of any load or store, while `rvfi_mem_wmask` and
`rvfi_mem_wdata` are non-zero only for stores; the store data is reported
in its byte lanes with the other lanes zero, the same form that goes to
memory. `halted` rises in the cycle after an ECALL, EBREAK or illegal
instruction has retired.

## 6. SoC

**IMEM** (`bram_sp`): 16 K x 32 single-port ROM, read with one cycle of
latency, output held while `en` (`imem_req`) is low. Contents come from
`IMEM_INIT` (`$readmemh`, used for the bitstream) or, in simulation, from
the harness's ELF loader. A fetch address outside the 64 KiB or not 4-byte
aligned drives the RAM's `clr` input, which loads 0 into its output register
instead of the word (the block RAM's synchronous output reset). The check
runs in the request cycle, alongside the RAM access, so the fetched word
passes through no extra logic; the only cost is the small address
comparator.

**DMEM** (`soc_dmem`): 64 K x 32 words in `BANKS = NPU_DMA_BYTES / 4`
word-interleaved banks (4 by default), each a `bram_tdp` true dual-port RAM
with byte enables. Word `w` lives in bank `w mod BANKS` at row `w / BANKS`.

```
  NPU_DMA_BYTES = 16:
  CPU (port A)   byte address [17:4] = row, [3:2] = bank
  NPU (port B)   byte address [17:4] = row, all four banks

             bank 0      bank 1      bank 2      bank 3
  row r    word 4r     word 4r+1   word 4r+2   word 4r+3
             |           |           |           |
  port B   [31:0]      [63:32]     [95:64]     [127:96]   one 16-byte block per cycle
  port A   only the addressed bank is enabled; its number, registered with the
           request, selects which bank's output is the CPU read data next cycle
```

Port A is the CPU's: it enables just the bank that holds the word and
registers the bank number, because the banks answer a cycle later; in that
cycle the registered number picks the read data, the same way `soc_bus`
picks DMEM over MMIO. Port B is the NPU's: an aligned block is one row
across all banks, so every bank is read or written in parallel and the NPU
moves `NPU_DMA_BYTES` bytes per cycle. Neither port stalls and both keep one
cycle of latency, so the CPU sees exactly the memory it saw before banking:
commit logs and cycle counts of every CPU-only program are identical to the
unbanked design. The banks add up to the same 256 KiB for every width, so
DMEM is 128 `DP16KD` block RAMs with 1, 2 or 4 banks.

Each array carries Yosys's `no_rw_check` attribute: the hardware gives no
guarantee when both ports touch the same word in one cycle, the software
contract never relies on one, and without the attribute Yosys would emulate
Verilog's cross-port read-old-data semantics in logic instead of using block
RAM. For the FPGA, `DMEM_INIT` is a path prefix and bank `b` loads
`<DMEM_INIT><b>.hex`; `tools/elf2hex.py --dmem PREFIX --dmem-banks N` writes
those files (and `--imem` the IMEM image).

**Decoder** (`soc_bus`): the CPU request goes to DMEM when the address is in
`0x1000_0000-0x1003_FFFF`, to MMIO in `0x2000_0000-0x2000_0FFF` and to the
NPU in `0x3000_0000-0x3000_0FFF`; anything else, including IMEM addresses
and the space past each region, reads 0 and drops writes, with no aliasing.
The selected source is registered and steers the read data in the next
cycle, when the slave answers. DMA addresses map onto DMEM port B as block
`(addr - 0x1000_0000) / NPU_DMA_BYTES`, with the same DMEM range check;
out-of-range DMA reads return 0 and writes are dropped.

**MMIO** (`soc_mmio`): `UART_TX`, `UART_STATUS`, `SIM_EXIT`, `LEDS`, decoded
by word offset; undefined offsets and the write-only registers read 0, and
a write must include byte lane 0. `uart_tx` is an 8N1 transmitter with
`UART_DIV` clocks per bit (default 217: 115200 baud at 25 MHz). In the
simulation build (`SIM = 1`) `UART_STATUS` reads 1 unconditionally, and the
harness takes bytes and the exit value from the write strobes
(`sim_uart_*`, `sim_exit_*`).

**NPU**: `soc_top` instantiates `npu_top` (`rtl/npu/`, pulled in by
`soc.f` through `-F ../npu/npu.f`) with `DMA_BYTES = NPU_DMA_BYTES`; its
register port hangs off the decoder and its DMA master owns DMEM port B, so
NPU traffic never stalls the core.

## 7. Simulator

`sim/` builds `build/sim/soc_sim` with `-O3 --x-assign fast --x-initial
fast`. Knobs select variants, and they combine:

| `make -C sim ...` | Binary | Difference |
|-------------------|--------|------------|
| `BP_MODE=0` | `soc_sim_bp0` | static not-taken prediction |
| `NPU_DMA_BYTES=4` (or 8) | `soc_sim_dma4` | narrower NPU port, fewer DMEM banks |
| `RANDRESET=1` | `soc_sim_randreset` | `--x-initial unique --x-assign unique`: every unreset flop and memory starts random (seed 1, or `+verilator+seed+N`) |
| `SERIAL_UART=1` | `soc_sim_uart` | `SIM = 0`, `UART_DIV = 5`: the real UART, with stdout decoded from `uart_txd` |

Tracing is compiled in and only enabled by `--vcd`. The command line and
the ELF checks are the ISS's (`tools/iss`): `--max-cycles` is decimal only,
`-h` exits 0, and `sim/elf_loader.cpp` rejects exactly what the ISS's
loader rejects (not an `ET_EXEC` RISC-V ELF32, a missing or truncated
program header table, a segment outside the file or not inside IMEM or
DMEM) with exit code 125, checking each segment before writing anything.
`+verilator+...` options go to Verilator through `commandArgs`. The loader
copies the `PT_LOAD` segments by physical address into zero-filled IMEM and
DMEM images, and the harness writes them into the model's memory arrays,
which `sim/sim.vlt` keeps public. It finds them through Verilator's scope
table by instance name, counting the DMEM banks the model was built with,
so the same harness works for every `NPU_DMA_BYTES`. The register file
starts at zero, like the ISS's.

In the serial build the harness samples every bit of the 8N1 frames in its
middle, keeps clocking after the run ends until the bytes of the retired
`UART_TX` stores have arrived, and reports in `--stats` how many
`UART_STATUS` reads found the transmitter busy. A byte written while the
UART is busy is dropped by the hardware, so a firmware bug in the
backpressure loop shows up as missing output on this build only. When the
line does not carry exactly the written bytes, or a frame is malformed, the
harness says so on stderr and a run that would exit with 0 exits with 1
(spec 7.1).

Each loop iteration observes one settled cycle and then clocks the model.
Within a cycle the retirement port (the WB instruction) is handled before
the MMIO write strobes (the MEM instruction), because WB holds the older
instruction. A UART byte is held from its store's strobe in MEM until that
store retires, and only then reaches stdout (in the serial build: counts as
written), so stdout always agrees with the commit log. The run ends right
after the `SIM_EXIT` store retires, so a younger UART store already in MEM
in that cycle does not print (`verif/core/uart.S` checks this), and neither
does one in MEM when `--max-cycles` runs out (`uart_budget.S`, cut at every
cycle by `budget-sweep`). The harness matches a retiring store to its byte
by the strobe's own condition, a store to the `UART_TX` word that writes
byte lane 0, so this holds whatever the distance from MEM to WB. A halt is
detected from `halted` in the cycle after the halting instruction retires;
ECALL/EBREAK and illegal instructions are told apart by the retired
instruction word.

Measured on an Apple M4 laptop (one thread): 11-16 M cycles/s for a
load/store/branch loop with a register-only NPU model (variation from machine load),
about 7 M cycles/s on CoreMark and MNIST with the real NPU in the model,
and about 4 M cycles/s while writing a commit log.

## 8. Verification

| What | Command | Result |
|------|---------|--------|
| riscv-tests rv32ui + rv32um, `BP_MODE = 1` | `make test-isa-rtl` | 48/48 |
| same, `BP_MODE = 0` | `make test-isa-rtl RTL_SIM=build/sim/soc_sim_bp0` | 48/48 |
| directed tests, both builds, with difftest and `--stats` checks | `make -C verif/core` | 32/32 runs |
| stdout agrees with retirement for every `--max-cycles` budget | `make -C verif/core budget-sweep` | 101 + 111 budgets (bp1 + bp0) |
| `soc_sim` and the ISS reject the same ELFs and command lines | `make -C verif/core cli` | 31/31 cases |
| riscv-tests and firmware from random initial state, seeds 1-3 | `make -C verif/core randreset` | 3 x (48 + 11) |
| predictor builds give identical commit logs | `make -C verif/core bp-compare` | 74/74 programs |
| firmware output over the real serial UART, every budget of `uart.S`, line errors fail the run | `make -C verif/core uart-serial` | 11/11 tests, 141 budgets |
| lint | `make lint` | clean |
| synthesis | `make -C rtl synth-check` | see section 9 |

The directed tests in `verif/core/` target what the riscv-tests do not
stress: forwarding from each stage into each operand, load-use into every
kind of consumer, back-to-back and dependent divides and the M-extension
corner cases, x0 under forwarding, BHT training patterns, wrong-path
instructions (including stores and halting instructions) being squashed,
BTB aliasing, call/return chains with changing return targets, the instret
semantics, SoC address decoding, end-of-run ordering, and halting, including
fetches outside IMEM, off its end and from a misaligned pc. Each
test checks its own results against constants (the M-extension values were
computed with an independent Python model of the ISA), and `check.sh` also
verifies the exit code, the UART output and that the commit log has exactly
`instret` lines. It runs every test that does not read `cycle` (all but
`csr.S` and `timing.S`) through `tools/difftest.py`, so its commit log must
match the ISS line for line, and checks the `--stats` counters: all
present, `cycles >= instret`, mispredicts no more than branches, and the
counter each test exists to exercise above zero (load-use stalls for
`loaduse.S`, mispredicts for `bpred.S` and `calls.S`, divider stalls for
`muldiv.S`). Only "above zero" is required, so these checks survive changes
to the hazard costs. `timing.S` checks the pipeline costs of sections 1-3
cycle-exactly, once per predictor mode.

`budget-sweep` (part of `make -C verif/core`) runs `uart_budget.S`, which
stores to `UART_TX` back to back, one and two instructions apart, after a
load, ahead of a divide and in a loop, through `difftest.py` with every
`--max-cycles` budget from 1 to the length of the run, on both predictor
builds. Every budget must pass, so wherever the budget cuts the run, the
RTL's stdout must be the ISS's after the same instructions. `uart-serial`
does the same for `uart.S` on the serial build, against its own commit log
(its `UART_STATUS` reads differ from the ISS's), and requires the harness
to fail `uart_budget.S`, which loses bytes on the line because it does not
poll.

While the tests were written, each was checked against deliberately broken
RTL (no MEM or WB forward, no load-use stall, no squash on redirect or halt,
x0 forwarded, instret missing in-flight instructions, a wrong divide-by-zero
sign, a wrong forwarding priority, no write-before-read, LB zero-extending,
a 31-step divider, no target check, a wrong BHT counter bit, and the
harness processing strobes before retirement); every mutant fails at least
one test, and several are caught by nothing but these tests.

The predictor must never change architectural behavior, so `bp-compare`
requires the commit logs of `soc_sim` and `soc_sim_bp0` to be identical for
every directed test, riscv-test and firmware test. The exceptions read
something that depends on time: `csr.S` reads `cycle`, and the NPU tests
poll `BUSY`, whose number of iterations depends on how fast the core runs.
For those only the UART output and exit code must match.

The `randreset` build catches state that is used before it is reset: with
the reset of the core's EX valid bit removed, the normal build still passes
all 48 riscv-tests (Verilator zeroes the flop), while the randomized build
fails all of them.

## 9. Results

CPI (`soc_sim --stats`); cycles include the 4-cycle pipeline fill:

| Program | `BP_MODE = 1` cycles / CPI | `BP_MODE = 0` cycles / CPI | Notes |
|---------|---------------------------|----------------------------|-------|
| `sw/bench/coremark` | 3544897 / 1.345 | 4126399 / 1.566 | 2.855 vs 2.453 CoreMark/MHz; 9.7% vs 58.8% of branches mispredicted |
| `sw/bench/gemm` | 13187098 / 1.021 | 14432925 / 1.118 | whole run, software and NPU GEMMs |
| `sw/tests/hello` | 1630 / 1.675 | 1908 / 1.961 | 973 instructions, 389 load-use bubbles, 198 divider stall cycles; 34 vs 173 mispredicts |
| `verif/core/calls` | 27491 / 1.199 | 32971 / 1.437 | recursive fib(15), returns to varying call sites |
| `verif/core/bpred` | 4093 / 1.179 | 5053 / 1.456 | loop and pattern branches |
| `rv32ui-p-add` | 526 / 1.143 | 496 / 1.078 | see below |
| `rv32ui-p-lw` | 314 / 1.129 | 302 / 1.086 | |
| `rv32um-p-div` | 394 / 4.330 | 394 / 4.330 | 9 divides, 297 divider stall cycles |

These are the cycle counts of the 40 MHz pipeline (section 10); its load
and multiply interlocks cost CPI that the clock more than pays back.

The riscv-tests run slightly *faster* without prediction: their bypass test
cases execute each check in a two-iteration loop, so the loop branch is
taken once (a miss in both modes) and then falls through, which the freshly
allocated, weakly-taken entry mispredicts. Loop-heavy code such as `hello`,
`calls` and `bpred` shows the benefit.

Yosys `synth_ecp5` of `soc_top`, NPU included, with the `hello` image
(`make -C rtl synth-check NPU_DMA_BYTES=4` and the default 16):

| Resource | `NPU_DMA_BYTES = 4` | `= 16` | Notes |
|----------|-------------------:|-------:|-------|
| LUT4 | 10228 | 10666 | with 2 per CCU2C, 14118 LUTs (16.9% of an ECP5-85F) at 16 |
| CCU2C | 1697 | 1726 | carry chains |
| TRELLIS_FF | 7323 | 7325 | |
| DP16KD | 165 | 173 | DMEM 128 in both, IMEM 29, NPU operand buffers 8 / 16 |
| TRELLIS_DPR16X4 | 92 | 92 | register file (16 per read port), BTB tags, targets and counters (60) |
| MULT18X18D | 70 | 70 | 4 in the core's multiplier, 66 in the NPU |

Banking leaves DMEM at 128 block RAMs; the extra block RAMs and most of the
extra LUTs at 16 bytes are the NPU's wider operand buffers and datapath
(`docs/npu.md`). IMEM is built from 32 one-bit-wide RAM columns; Yosys drops
the columns whose bit is constant across the whole image (3 for `hello`),
so a full program uses 32. The core alone (`rv32_core`) is 3538 LUT4, 396
CCU2C, 1250 flip-flops and 92 `DPR16X4`, with no block RAM.

What the 16-byte port buys (`soc_sim` vs `soc_sim_dma4`, `sw/bench`):

| Benchmark | `NPU_DMA_BYTES = 4` | `= 16` |
|-----------|--------------------:|-------:|
| MNIST, NPU cycles per inference | 5680 (61.9x software) | 3015 (116.5x) |
| GEMM 64 x 64 x 256, NPU cycles | 65213 (56.1x) | 38141 (96.0x) |
| GEMM 16 x 16 x 2048, NPU cycles | 42135 (42.2x) | 17687 (100.5x) |
| GEMM 8 x 8 x 64, NPU cycles | 665 (23.9x) | 489 (32.5x) |
| CoreMark | 3.29 / MHz | 3.29 / MHz (identical cycles) |

These were measured with the 25 MHz pipeline that preceded section 10 and
an earlier NPU driver; with the current pipeline and driver the four
16-byte NPU figures are 3053, 38929, 17766 and 527 cycles.

## 10. Clock frequency and CPI

On the ULX3S's LFE5U-85F ([fpga.md](fpga.md)) the original pipeline closed
timing at 25 MHz and no further: 26.3-27.8 MHz over eight nextpnr seeds.
Every critical path started at a block RAM output: the load data from DMEM,
forwarded from WB into EX, and the instruction word from IMEM, through the
decode and the interlock to the stall. The changes below took those paths
off the RAM outputs one at a time. Each step was placed and routed with
seeds 1-4 against a 50 MHz target (`make -C fpga seeds CLK_MHZ=50`) and
checked to leave every cycle count unchanged except where its hazard rule
changed:

| Step | Change | Fmax, seeds 1-4 (MHz) | Critical path afterwards |
|------|--------|----------------------:|--------------------------|
| 0 | the 25 MHz pipeline | 26.3-27.8 (8 seeds) | DMEM load in WB -> forward -> 33 x 33 multiply -> `res_m`, 37 ns |
| 1 | multiply in MEM, mul-use interlock (section 3) | 27.3-28.3 | DMEM load -> forward -> JALR adder -> target compare -> redirect -> next PC -> BTB RAM address |
| 2 | load data not forwarded (section 1.1) | 33.6-38.4 | IMEM -> decode -> interlock -> stall -> next PC -> BTB RAM address |
| 3 | IMEM reads every cycle, skid register (section 1) | 36.0-40.0 | the same, without the IMEM clock enable |
| 3b | interlock operand flags from the opcode alone | 36.9-39.5 | the same, shorter decode |
| 4 | BTB in LUT RAM, predicted-target compare in EX | 40.1-44.0 | IMEM -> decode -> interlock -> stall -> `pc_f` and its copies |
| 5 | interlock as a lookup in one-hot masks | 43.7-46.3 | the same, 21.5-23 ns |

At its 40 MHz constraint the final pipeline reaches 42.1-46.5 MHz over
eight seeds (worst slack 1.2 ns), and the board runs it at 40 MHz from the
PLL. The cycles it gives up are the extra load-use bubbles (on CoreMark
323,168 -> 762,299: a second bubble after every adjacent load, plus 115,963
for consumers one instruction later) and about 30,000 mul-use bubbles
(1%):

| | Before: 25 MHz | After: 40 MHz | |
|---|---:|---:|---:|
| CoreMark CPI, `BP_MODE = 1` / `0` | 1.167 / 1.388 | 1.345 / 1.566 | +15% / +13% |
| CoreMark/MHz, `BP_MODE = 1` / `0` | 3.291 / 2.768 | 2.855 / 2.453 | -13% / -11% |
| **CoreMark/s, `BP_MODE = 1`** | **82.3** | **114.2** | **1.39x** |
| CoreMark/s, `BP_MODE = 0` | 69.2 | 98.1 | 1.42x |
| MNIST on the NPU, cycles per inference | 3027 | 3053 | +0.9% |
| **MNIST on the NPU, us per inference** | **121.1** | **76.3** | **1.59x** |
| MNIST in software, us per inference | 14055 | 8798 | 1.60x |
| GEMM 64 x 64 x 256 on the NPU, us | 1534 | 973 | 1.58x |

The NPU workloads keep almost all of the clock gain because they spend
their time in the NPU, not in the interlocks.

What limits the clock now is the cone from the IMEM output to the stall:
5.6 ns of block RAM clock-to-out, about 3 ns of routing out of IMEM, about
ten LUT levels of decode and mask lookup, then the next-PC mux of a copy of
`pc_f` that Yosys builds as the read-address register of the BTB LUT RAM
(where the stall becomes a mux select rather than a clock enable). Going
further would take a decoupled fetch stage (an instruction queue, so that a
stall no longer holds the fetch PC), and next the multiply in MEM, about
20 ns, would need a second stage and a second mul-use bubble. Each is a
larger change for at most 10-15% more clock, so the pipeline stops here.
The compiler is also not told about the longer load latency; a scheduling
model for it could recover some load-use bubbles in software.
