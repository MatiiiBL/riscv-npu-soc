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
| `rtl/core/rv32_muldiv.sv` | single-cycle multiplier, iterative divider |
| `rtl/core/rv32_bpred.sv` | BTB with 2-bit counters (or nothing, for `BP_MODE = 0`) |
| `rtl/core/rv32_core.sv` | the five pipeline stages, hazards, CSRs, counters, retire port |
| `rtl/soc/soc_top.sv` | core + IMEM + DMEM + decoder + MMIO + NPU |
| `rtl/soc/bram_sp.sv`, `bram_tdp.sv` | inferable single-port ROM and true dual-port RAM |
| `rtl/soc/soc_bus.sv` | address decoder for the CPU data port and the NPU DMA port |
| `rtl/soc/soc_mmio.sv`, `uart_tx.sv` | UART, SIM_EXIT, LEDS |
| `rtl/soc/npu_stub.sv` | placeholder `npu_top` until the real NPU is integrated |

## 1. Pipeline

```
            IF                  ID                     EX                       MEM               WB
     +--------------+   +-----------------+   +-----------------------+   +--------------+   +--------------+
     | pc_f         |   | decode          |   | forwarding muxes      |   | dmem request |   | load align / |
     | BTB lookup   |   | regfile read    |   | ALU, branch compare   |   | byte enables |   |  sign extend |
     | next-PC mux  |   | imm, pc + imm   |   | redirect, BTB update  |   | store lanes  |   | regfile write|
     |              |   | load-use check  |   | MUL, DIV FSM, CSRs    |   |              |   | rvfi_*       |
     +------+-------+   +--------+--------+   +-----------+-----------+   +------+-------+   +------+-------+
            |  imem_addr         ^ imem_rdata             |                      |                  |
            v                    |                        |                      |                  |
        [ IMEM BRAM ]------------+                        |                      |                  |
            ^                                             |                      |                  |
            +-------------- redirect_pc_x ----------------+                      |                  |
                                         EX/MEM result ---> EX operands <--------+                  |
                                         MEM/WB result ---> EX operands <---------------------------+
                                         WB write  -------> regfile (seen by ID in the same cycle)
```

Signals carry a stage suffix (`_f`, `_d`, `_x`, `_m`, `_w`); every pipeline
register has a `valid` bit, and a bubble is simply `valid = 0`.

**IF.** `pc_f` is presented to IMEM and, in the same cycle, to the branch
predictor. Because IMEM has one cycle of latency, its output register *is*
the IF/ID instruction register: ID decodes `imem_rdata` directly, and only
the PC and the prediction are stored in flops. Holding ID therefore means
deasserting `imem_req`, which makes the block RAM keep its output (spec 4.1).

**ID.** Decode, register-file read, and two things that do not depend on
registers and so are taken out of EX: the branch/JAL target `pc + imm` and
the comparison of that target with the predicted one. The load-use check is
also done here, against the instruction in EX.

**EX.** Forwarding muxes, ALU, branch comparator, misprediction check and
redirect, BTB update, multiplier, divider, and CSR reads. The result written
to `rd` (ALU, link `pc + 4`, CSR, MUL or DIV) is selected here.

**MEM.** The load/store request goes out on the data port with byte enables
and store data already shifted into its lanes (`dmem_be`, `dmem_wdata`).

**WB.** Load data comes back from the SoC (steered by the decoder), is
aligned and extended, and is written to the register file; the instruction
is reported on the retirement port.

### 1.1 Data hazards and forwarding

| Producer distance | Path | Cost |
|-------------------|------|------|
| 1 (producer in MEM) | EX/MEM `res_m` -> EX operand mux | 0 |
| 2 (producer in WB) | MEM/WB `wb_data_w` -> EX operand mux | 0 |
| 3 (producer writing back while consumer is in ID) | register file write-before-read | 0 |
| load, distance 1 | stall ID one cycle, then MEM/WB forward | 1 bubble |

The forwarding rule for each EX operand is: take the MEM-stage result if the
instruction in MEM is valid, writes a register (`rd != x0`) and `rd`
matches; otherwise the WB-stage result under the same conditions; otherwise
the value read in ID. MEM has priority because it is the younger producer.
`rd_we` is computed in the decoder as "writes a register *and* `rd != 0`",
so writes to x0 never enter the bypass network at all, and the register file
forces reads of x0 to zero.

Load data only exists in WB, so a load followed directly by an instruction
that reads its result cannot be served by the MEM forward. The decoder
reports which source registers an instruction really reads (`uses_rs1`,
`uses_rs2`; for example LUI, JAL and the CSR instructions read none), and ID
stalls for one cycle only when such an operand matches the `rd` of a load in
EX:

```
cycle            1    2    3    4    5    6
lw   a1, 0(s0)   IF   ID   EX   MEM  WB
add  a2, a1, a3       IF   ID   ID   EX   MEM     a1 forwarded from MEM/WB in cycle 5
next                  IF   IF   ID   EX            (bubble in EX in cycle 4)
```

This covers every consumer the same way: ALU operands, branch operands,
store data, store and load addresses, `jalr` bases and divider operands.
Because the stall guarantees that a load's consumer is never in EX while the
load is in MEM, the MEM-stage forward never has to select load data, which
keeps the data-memory read path out of the EX operand muxes.

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

Branch and jump targets that do not depend on registers are computed in ID,
and the predicted-target comparison for them is registered there too, so
only JALR has an adder in the EX redirect path.

### 1.3 Halting

ECALL, EBREAK and every encoding outside RV32IM/Zicsr decode as `is_halt`
with no other effect. When such an instruction reaches EX (where it is known
to be on the correct path, since only EX can redirect), it flushes ID and IF
and sets a sticky flag that stops fetching. It then retires normally from WB
(logged with `rd = 00`), and `halted` rises the following cycle. Nothing
younger reaches MEM, so a store behind an ECALL never happens.

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

On FPGA the tag and target arrays map to two block RAMs (Yosys moves the
`pc_f` register into the RAM's address register) and the counters to LUT
RAM.

## 3. Multiply and divide

**MUL, MULH, MULHSU, MULHU** are a single combinational multiply in EX. The
operands are sign- or zero-extended to 64 bits according to the variant and
multiplied with a 64-bit result: the exact 33 x 33 product always fits, so
one multiplier serves all four instructions (MUL takes bits 31:0, the others
63:32). Verilator evaluates it with native 64-bit arithmetic, and Yosys
narrows the redundant sign bits back to a 33 x 33 multiply on four ECP5
`MULT18X18D` blocks plus a carry-chain adder.

*Why single-cycle rather than pipelined:* a pipelined multiplier would
produce its result at the end of MEM, which would need a second interlock
("mul-use", like load-use) and cost a bubble on every dependent multiply,
which is common in address arithmetic and in the CPU fallback paths of the
GEMM kernels. The board clock of the target (ULX3S) is 25 MHz, a 40 ns
budget in which a DSP-based multiply plus its adder is expected to fit. The
multiplier is the first candidate to split across EX/MEM if a faster clock
is targeted later; it would be a local change (an extra `is_mul` check next
to the load-use one). Timing has not been closed with nextpnr yet.

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
| `perf_load_use` | load-use bubbles |
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
the harness's ELF loader.

**DMEM** (`bram_tdp`): 64 K x 32 true dual-port RAM with byte enables; port
A serves the CPU and port B the NPU DMA. The array carries Yosys's
`no_rw_check` attribute: the hardware gives no guarantee when both ports
touch the same word in one cycle, the software contract never relies on
one, and without the attribute Yosys would emulate Verilog's cross-port
read-old-data semantics in logic instead of using block RAM. `DMEM_INIT`
initialises it for the FPGA. `tools/elf2hex.py` turns an ELF into both
images.

**Decoder** (`soc_bus`): the CPU request goes to DMEM when the address is in
`0x1000_0000-0x1003_FFFF`, to MMIO in `0x2000_0000-0x2000_0FFF` and to the
NPU in `0x3000_0000-0x3000_0FFF`; anything else, including IMEM addresses
and the space past each region, reads 0 and drops writes, with no aliasing.
The selected source is registered and steers the read data in the next
cycle, when the slave answers. DMA addresses map onto DMEM port B the same
way (`(addr - 0x1000_0000) >> 2`); out-of-range DMA reads return 0.

**MMIO** (`soc_mmio`): `UART_TX`, `UART_STATUS`, `SIM_EXIT`, `LEDS`, decoded
by word offset; undefined offsets and the write-only registers read 0, and
a write must include byte lane 0. `uart_tx` is an 8N1 transmitter with
`UART_DIV` clocks per bit (default 217: 115200 baud at 25 MHz). In the
simulation build (`SIM = 1`) `UART_STATUS` reads 1 unconditionally, and the
harness takes bytes and the exit value from the write strobes
(`sim_uart_*`, `sim_exit_*`).

**NPU slot**: `npu_stub.sv` provides an `npu_top` with the spec ports whose
registers are plain storage, `INFO` and `CYCLES` read 0, and `START` sets
`DONE | ERR` at once with no DMA. `soc.f` lists it until the real NPU
(`rtl/npu/npu.f`) replaces it.

## 7. Simulator

`sim/` builds `build/sim/soc_sim` (and `soc_sim_bp0` with
`make -C sim BP_MODE=0`) with `-O3 --x-assign fast --x-initial fast`;
tracing is compiled in and only enabled by `--vcd`. The ELF loader in
`sim/elf_loader.cpp` copies the `PT_LOAD` segments by physical address into
zero-filled IMEM and DMEM images and writes them into the model's memory
arrays, which `sim/sim.vlt` keeps public for that purpose. The register file
starts at zero, like the ISS's.

Each loop iteration observes one settled cycle and then clocks the model.
Within a cycle the retirement port (the WB instruction) is handled before
the MMIO write strobes (the MEM instruction), because WB holds the older
instruction: the run ends right after the `SIM_EXIT` store retires, and a
younger UART store that is already in MEM in that cycle must not reach
stdout (`verif/core/uart.S` checks this). A halt is detected from `halted`
in the cycle after the halting instruction retires; ECALL/EBREAK and
illegal instructions are told apart by the retired instruction word.

Measured on an Apple M4 laptop (one thread): 11-16 M cycles/s for a
load/store/branch loop with the NPU stub (variation from machine load),
about 7 M cycles/s on CoreMark and MNIST with the real NPU in the model,
and about 4 M cycles/s while writing a commit log.

## 8. Verification

| What | Command | Result |
|------|---------|--------|
| riscv-tests rv32ui + rv32um, `BP_MODE = 1` | `make test-isa-rtl` | 48/48 |
| same, `BP_MODE = 0` | `make test-isa-rtl RTL_SIM=build/sim/soc_sim_bp0` | 48/48 |
| directed tests, both builds | `make -C verif/core` | 24/24 runs |
| lint | `make lint` | clean |
| synthesis | `make -C rtl synth-check` | see section 9 |

The directed tests in `verif/core/` target what the riscv-tests do not
stress: forwarding from each stage into each operand, load-use into every
kind of consumer, back-to-back and dependent divides and the M-extension
corner cases, x0 under forwarding, BHT training patterns, wrong-path
instructions (including stores and halting instructions) being squashed,
BTB aliasing, call/return chains with changing return targets, the instret
semantics, SoC address decoding, end-of-run ordering, and halting. Each
test checks its own results against constants (the M-extension values were
computed with an independent Python model of the ISA), and `check.sh` also
verifies the exit code, the UART output and that the commit log has exactly
`instret` lines. `timing.S` checks the pipeline costs of sections 1-3
cycle-exactly, once per predictor mode.

While the tests were written, each was checked against deliberately broken
RTL (no MEM or WB forward, no load-use stall, no squash on redirect or halt,
x0 forwarded, instret missing in-flight instructions, a wrong divide-by-zero
sign, a wrong forwarding priority, no write-before-read, LB zero-extending,
a 31-step divider, no target check, a wrong BHT counter bit, and the
harness processing strobes before retirement); every mutant fails at least
one test, and several are caught by nothing but these tests.

The predictor must never change architectural behavior, so the commit logs
of `soc_sim` and `soc_sim_bp0` are identical for every program (checked for
all riscv-tests, the directed tests and `hello`).

## 9. Results

CPI (`soc_sim --stats`); cycles include the 4-cycle pipeline fill:

| Program | `BP_MODE = 1` cycles / CPI | `BP_MODE = 0` cycles / CPI | Notes |
|---------|---------------------------|----------------------------|-------|
| `sw/bench/coremark` | 3075804 / 1.167 | 3657306 / 1.388 | 3.29 vs 2.77 CoreMark/MHz; 9.7% vs 58.8% of branches mispredicted |
| `sw/bench/gemm` (CPU parts) | 13243977 / 1.015 | 14490115 / 1.116 | |
| `sw/tests/hello` | 825 / 1.230 | 915 / 1.364 | 671 instructions, 98 load-use bubbles; 27 vs 72 mispredicts |
| `verif/core/calls` | 27480 / 1.198 | 32960 / 1.437 | recursive fib(15), returns to varying call sites |
| `verif/core/bpred` | 4091 / 1.179 | 5051 / 1.455 | loop and pattern branches |
| `rv32ui-p-add` | 526 / 1.143 | 496 / 1.078 | see below |
| `rv32ui-p-lw` | 310 / 1.115 | 298 / 1.072 | |
| `rv32um-p-div` | 394 / 4.330 | 394 / 4.330 | 9 divides, 297 divider stall cycles |

The riscv-tests run slightly *faster* without prediction: their bypass test
cases execute each check in a two-iteration loop, so the loop branch is
taken once (a miss in both modes) and then falls through, which the freshly
allocated, weakly-taken entry mispredicts. Loop-heavy code such as `hello`,
`calls` and `bpred` shows the benefit.

Yosys `synth_ecp5` of `soc_top` with the `hello` image
(`make -C rtl synth-check`):

| Resource | Count | Notes |
|----------|-------|-------|
| LUT4 | 4380 | about 5% of an ECP5-85F |
| TRELLIS_FF | 1577 | |
| CCU2C | 400 | carry chains |
| DP16KD | 159 | 128 DMEM, 29 IMEM, 2 BTB |
| TRELLIS_DPR16X4 | 36 | register file (16 per read port), BTB counters (4) |
| MULT18X18D | 4 | multiplier |

IMEM is built from 32 one-bit-wide RAM columns; Yosys drops the columns
whose bit is constant across the whole image (3 for `hello`), so a full
program uses 32. The core alone (`rv32_core`) is 3246 LUT4 and 1115
flip-flops.
