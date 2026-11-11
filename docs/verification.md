# Pulse verification

Pulse is checked at several levels. Each level has its own reference, so a bug
has to slip past more than one independent check to reach the board:

| Level | What runs | Checked against | Location | Command |
|-------|-----------|-----------------|----------|---------|
| ISA compliance | riscv-tests `rv32ui` + `rv32um` (48 tests) | self-checking tests | `verif/riscv-tests/` | `make test-isa` |
| Core, directed | short pipeline-hazard programs | their own constants, the ISS through difftest, `--stats` counters, the cycle costs of spec section 6 | `verif/core/` | `make test-core` |
| Simulator variants | both predictor builds, random initial state, the real serial UART, 4- and 8-byte NPU DMA ports | each other, and the riscv-tests and firmware checks | `verif/core/` | `make test-variants` |
| Core, random | constrained-random RV32IM programs | ISS, commit log line by line | `verif/random/`, `tools/rvgen/`, `tools/difftest.py` | `make test-random` |
| NPU unit | cocotb tests of `npu_top` | Python golden model | `verif/npu/` | `make test-npu` |
| Firmware | C tests, including NPU driver tests | `expected.txt`, on both simulators; the NPU tests also on the ISS for ARR 1 to 8 crossed with six KMAX values (`make -C sw npu-sweep`), by their own software reference | `sw/tests/` | `make test-sw` |
| Reference model | ISS decode/execute and NPU model | hand-computed values | `tools/iss/test/` | `make test-iss` |
| RTL hygiene | Verilator lint | `-Wall` clean | `rtl/` | `make lint` |

`make test` runs every row of this table. Every target exits nonzero on a
failure.

`docs/spec.md` is the contract all of these are written against. When two
models disagree, the spec decides which one is wrong, and when the spec does
not decide, it gets a sentence that does.

## 1. The reference ISS

`tools/iss` (built as `build/iss/iss`) is a C++17 instruction-set simulator of
the whole SoC: the RV32IM + Zicsr core, the memory map of spec section 3 (UART,
`SIM_EXIT`, `LEDS`, unbacked addresses that read 0), and a functional model of
the NPU that follows the pseudocode of spec section 5.1 step by step. It takes
the same command line as the RTL simulator and writes the same commit log
(spec section 7), so the two can be compared directly.

The ISS differs from the RTL only where the spec says timing is
implementation-defined:

- It has no clock. `cycle` and `time` read the same value as `instret`, and
  `--max-cycles` counts retired instructions.
- An NPU operation completes inside the store that sets `START`: `STATUS`
  never reads `BUSY` and `CYCLES` reads 0.

For behavior the spec leaves undefined, the ISS picks something easy to
diagnose. A misaligned load or store uses the lanes of the containing word and
prints a warning at exit. A fetch outside IMEM or from a misaligned pc halts
as an illegal instruction (exit code 3).

The whole IMEM is decoded once at reset, which is safe because IMEM is not
writable from the data port. The execute loop is a single switch, with the
commit-log path compiled separately. On an Apple Silicon laptop,
`make -C tools/iss bench` (500 million instructions of CRC, matrix multiply,
division and sorting firmware) runs at about **760 MIPS**, or **165 MIPS** with
the commit log being formatted.

`make -C tools/iss test` runs about 800 checks with hand-computed expected
values: every ALU and M-extension corner case (division by zero,
`INT_MIN / -1`, `MULH*` signedness), immediate encodings cross-checked against
`llvm-mc`, reserved encodings that must decode as illegal, load/store lanes and
sign extension, the memory map, counter CSRs, halts, the exact commit-log text
of the spec example, the ELF loader, and for the NPU: rounding of negative
values, saturation, `SHIFT = 31` with 48-bit products, strides, INT8 byte
enables, `ACC_KEEP`/`NO_WB`, accumulator wrap-around and every `ERR` rule.
Mutating the model (dropping the rounding term, ignoring ReLU, writing the
wrong byte enables, reading `instret` one too high, ...) makes these tests
fail, so they actually pin the behavior down.

## 2. ISA compliance: riscv-tests

`verif/riscv-tests/` builds the official `rv32ui` and `rv32um` suites against
a bare-metal environment (`env/riscv_test.h`) that replaces the trap-based
upstream one: a test reports by storing to `SIM_EXIT`, 0 for a pass or
`(TESTNUM << 1) | 1` for a failure, so the exit code names the failing case.
`fence_i` (self-modifying code; IMEM is fetch-only) and `ma_data` (misaligned
accesses) are excluded.

```sh
make test-isa-iss          # 48 tests on the ISS
make test-isa-rtl          # 48 tests on the RTL
```

Per-test output is in `build/riscv-tests/<test>.{iss,rtl}.log` and the
disassembly in `build/riscv-tests/<test>.dis`.

## 3. Directed core tests

The random tests find hazards statistically. The directed tests in
`verif/core/` (`make test-core`; `docs/core.md` section 8 lists them) hit each
one on purpose: forwarding from every stage into every operand, load-use into
every kind of consumer, dependent divides and the M-extension corner cases,
x0 under forwarding, branch-predictor training, squashing of wrong-path
instructions, call/return chains, `instret`, SoC address decoding, and every
way to halt, bad fetches included. Each program checks its own results
against constants and reports through `SIM_EXIT`. On both predictor builds,
`verif/core/check.sh` also requires of every run:

- the expected exit code and UART output, and a commit log of exactly
  `instret` lines;
- the same commit log, stdout and exit code from the ISS through
  `tools/difftest.py`, for every test that does not read `cycle` (all but
  `csr.S` and `timing.S`);
- consistent `soc_sim --stats` counters (cycles at least instret, no more
  mispredicts than branches) and, above zero, the counter the test exists to
  exercise: load-use stalls for `loaduse.S`, mispredicts for `bpred.S` and
  `calls.S`, divider stall cycles for `muldiv.S`.

`timing.S` measures the costs of spec section 6 cycle-exactly with two
`rdcycle` reads around each case, once per predictor mode: one bubble per
load-use pair, two per mispredicted branch, none for a correctly predicted
taken branch, and 34 cycles in EX for a divide. `make test-core` finally runs
`verif/core/sim_cli.py`, which requires `soc_sim` and the ISS to accept and
reject the same command lines and malformed ELF files with the same exit
codes (spec 7.1).

`make test-variants` checks the simulator builds whose behavior must not
differ from the default one:

- `bp-compare`: `soc_sim` and `soc_sim_bp0` must give identical commit logs
  for every directed test, riscv-test and firmware test. Programs that read
  `cycle` or poll the NPU's `BUSY` flag retire a timing-dependent number of
  instructions, so for them only the output and exit code must match.
- `randreset`: a build whose unreset flops and memories start random
  (`--x-initial unique`) runs the riscv-tests and the firmware tests for
  three seeds, so nothing may depend on state that reset does not set. The
  harness still writes the program into IMEM and DMEM and zeros the
  register file, so what stays random is the state it does not write, such
  as the BTB's tag, target and counter arrays and the NPU's operand buffers.
- `uart-serial`: a build with the real UART (`SIM = 0`) must deliver every
  firmware test's output intact over the serial line, and `hello` must have
  found the transmitter busy, so the backpressure path is exercised.
- the firmware tests on `soc_sim_dma4` and `soc_sim_dma8`, whose NPU DMA
  port moves 4 or 8 bytes per access instead of 16 (DMEM in 1 or 2 banks
  instead of 4), so the NPU reads its operands in more, smaller blocks.

## 4. Random differential testing

This is the main check of the core: generate a program, run it on the RTL and
on the ISS, and require the two commit logs to be identical.

### 4.1 Generator

`tools/rvgen/rvgen.py --seed S --length N -o prog.S` writes a self-contained
program (its own `_start`, no crt0 or libc) of about `N` instructions. It
builds with the normal firmware flags, `sw/common/link.ld` and
`-Isw/common`. By construction, every program:

- **terminates.** Control flow only moves forward, except loops closed by a
  reserved counter register that the loop body cannot write (1 to 5
  iterations, counting up or down, closed by `bne`/`blt`/`bltu`), and calls to
  straight-line subroutines that return through a link register their bodies
  cannot write. Branch targets are placed only between whole snippets, so no
  branch can jump into a loop and skip its counter setup.
- **stays inside defined behavior.** Loads and stores are aligned. Most go
  to a 4 KiB scratch buffer in DMEM (the base register points at its middle,
  so offsets go both ways); the rest go to fixed addresses, listed below.
  Programs never read `cycle` or `time` and never start the NPU, so nothing
  depends on timing. The random flow fails any seed in which the ISS sees a
  misaligned access, so this promise is checked on every run.
- **is deterministic.** A private splitmix64 PRNG makes the output depend only
  on `--seed` and `--length`, not on the Python version. The buffer contents
  come from a separate stream, so a shorter `--length` produces a prefix of the
  same program with the same data and the same ending (see 4.4).
- **ends in a known way.** 70 % of the seeds end with `sw zero` to
  `SIM_EXIT`. Another 3 % do too, but first run code placed in the last
  words of IMEM one to three times, whose last word jumps back: the
  sequential fetch past it, from `0x10000`, is on the wrong path and must not
  halt, whether the predictor is cold or trained. The rest end with `ECALL`
  or `EBREAK` (exit 2), a reserved encoding (`0`, all ones, `MRET`, `WFI`, a
  custom opcode, an RV64-only shift; exit 3), or a bad fetch (exit 3, spec
  2): a `JALR` outside IMEM; a `JALR`, `JAL` or taken branch to a target 2
  bytes past an instruction boundary; or falling off the end of IMEM, from
  the same end-of-IMEM code with a branch that falls through on its last
  pass after the predictor learned it as taken. The file header names the
  exit code, which the random flow requires from both models; `--ending`
  forces one.

The program is a random sequence of snippets, chosen to be hazard-dense:

- every ALU register and immediate op, `LUI`/`AUIPC`, shifts by 0, 31 and
  random amounts, register shifts whose upper bits are set;
- every M op, often on corner operands (0, ±1, `INT_MIN`, `INT_MAX`, ...)
  loaded right before, with the result used by the next instruction;
- loads and stores of every width and signedness, often to a recently
  touched word, a store read back at a different width, addresses computed
  by the preceding `addi`, and a pointer loaded from memory and used at once;
- a loaded value used immediately by an ALU op, a branch, a store (as data)
  or a `JALR` (as the target);
- forward branches of all six kinds on equal, zero, fresh or random operands,
  and never-taken branches to a misaligned target, which must not halt;
  `JAL`, and `JALR` with offsets, with bit 0 set, with `rd == rs1`, and through
  a target loaded from memory;
- dependent chains of 2 to 5 ALU/M ops, writes to `x0`, `instret`/`instreth`/
  `minstret` reads, writes to CSRs that must be ignored, reads of CSRs that
  must return 0, `FENCE`/`FENCE.I`, and occasional UART writes (so stdout is
  compared too);
- word accesses to the NPU registers (writes read back with all 32 bits,
  `STATUS`, `INFO`, `CYCLES` and write-only `CTRL` reads, `CTRL` writes with
  `START` clear), `LEDS` (8 bits read back) and `UART_STATUS`;
- addresses that must read 0 and drop writes: undefined MMIO and NPU offsets,
  the last word of each window and the words just past it (a store to the
  would-be alias of `SIM_EXIT` or `CTRL` must do nothing), IMEM through the
  data port, just past IMEM and DMEM, and the unmapped regions, at every
  width; DMEM's first and last words; and a store over the next instruction,
  which must not change what executes.

Over 40 seeds of 2000 instructions (about 100,000 executed instructions) every
branch type is both taken and not taken hundreds of times, backward branches
are taken about 2600 times, and there are about 4500 load-use pairs (into
ALU/address, branch, store data and `JALR`). Of the 19,600 loads and stores,
about 1100 go to MMIO, 620 to NPU registers, 330 to IMEM and 160 to unmapped
regions. The base and counter registers are picked per seed, so no
register is permanently left out of the hazard mix.

### 4.2 Comparison

`tools/difftest.py --rtl <sim> --iss <iss> prog.elf` runs both simulators with
`--commit-log` and compares the commit logs line by line, stdout byte for
byte, and the exit codes. Exit status 0 means a match, 1 a mismatch, and 2 a
tool error such as a missing simulator. Simulator commands are split like shell
words, so they can carry extra flags (`--rtl 'build/sim/soc_sim --stats'`).
`--work-dir DIR` keeps the logs, and `--expect-exit 0` also fails a run where
both models agree on a nonzero exit.
`llvm-objdump` comes from `--objdump`, then `$RV_OBJDUMP`, then the path
`mk/toolchain.mk` detects, then `PATH`.

`--max-cycles N` is the RTL's budget in clock cycles; the ISS counts retired
instructions instead (spec 7.1). difftest runs the RTL first and gives the ISS
an instruction limit of `N`, which can never stop it first because the RTL
retires at most one instruction per cycle. If the RTL uses up its budget, the
ISS is stopped after exactly as many instructions as the RTL retired, so both
logs cover the same instructions and both runs end with 124; the report says
so, and `--expect-exit 0` makes it a failure.

A commit-log comparison is exact, so it only applies to programs whose
instruction stream does not depend on timing: no `cycle`/`time` reads and no
polling of NPU `STATUS`, since the RTL stays busy for many cycles and its poll
loop retires more instructions. Firmware that does either is compared on its
output instead (section 6).

### 4.3 Running it

```sh
make test-random                          # builds both simulators, 200 seeds
make -C verif/random -j8                  # same, in parallel
make -C verif/random SEEDS=2000 LEN=6000  # a longer regression
make -C verif/random SEED_START=5000 SEEDS=1   # one seed
make -C verif/random RTL_SIM=build/sim/soc_sim_bp0   # static branch prediction
make -C verif/random RTL_SIM=build/iss/iss           # ISS on both sides (checks the flow)
make -C verif/random selftest             # checks that difftest catches faults
```

The run prints one line per failing seed and a summary, for example
`random difftest: 200 passed, 0 failed (seeds 1..200, LEN=2000, ...)`. A
failing seed keeps its `.S`, `.elf`, both commit logs, stdout, stderr and the
difftest report in `build/random/fail/`.

`make selftest` tests the tester. `fault_sim.py` wraps the ISS and corrupts its
result the way a broken RTL would: a wrong `rd_wdata`, store data, load
address or pc, a dropped, duplicated or missing instruction, a changed UART
byte, a different exit code. `selftest.py` checks that difftest flags each fault at the right
line and field, and that the unmodified run matches.

### 4.4 Debugging a mismatch

A report looks like this (shortened; produced by `make selftest`, which
flipped one bit of a result):

```
difftest FAIL: build/random/selftest.elf
  commit log differs at line 1294 (1293 instructions matched)
              1290  00000fc8 2f523213 04 00000000
              1291  00000fcc 0045b5b3 11 00000000
              1292  00000fd0 0f8011ef 03 00000fd4
              1293  000020c8 00000897 17 000020c8
    rtl >     1294  000020cc f9888893 17 00002061
    iss >     1294  000020cc f9888893 17 00002060
          fields: rd_wdata
    rtl   then    000020d0 07c88ce7 25 000020d4
    iss   then    000020d0 07c88ce7 25 000020d4
  pc 000020cc: <sub3> f9888893  addi a7, a7, -0x68
  logs: /tmp/difftest-selftest-.../selftest.{rtl,iss}.{commit,stdout,stderr}
```

1. **Read which fields differ.** Line *n* is the *n*-th retired instruction.
   - `rd_wdata`: a datapath result is wrong. Find the instructions that
     produced its source registers in the lines above (`--context 30` shows
     more). If a producer is only one or two lines back, suspect forwarding.
     If it is a load, suspect the load-use stall or lane extraction. If it is
     a divide, suspect the divider handshake.
   - `pc`: control flow went wrong. The previous line is usually a branch or
     jump: check its resolution, the redirect, and the flush of the wrong-path
     instructions.
   - an `rtl` log that runs ahead (an extra line) or behind (a missing line):
     the retirement logic dropped or duplicated an instruction, usually
     around a stall or a flush. The `then` lines show which.
   - `mem_addr`: the address of a load or store (every memory access is
     logged with its word address, spec 7.2), so suspect forwarding into the
     address or the immediate. A load can return the right data from the
     wrong address, which only this field shows.
   - `wmask`, `wdata`: a store's byte enables or store-data forwarding.
   - `the rtl log ends here`: the RTL stopped early (illegal instruction,
     `--max-cycles`); its stderr is at the end of the report.
   - only `stdout` or the exit code differs: the UART or `SIM_EXIT` path.
2. **Find the source.** The disassembly line names the rvgen label (`fwd*`
   forward targets, `loop*`, `sub*`) and the offset from it; search for it in
   the `.S` file in `build/random/fail/`.
3. **Shrink the program.** Rerun the seed with a smaller `--length`. The
   result is a prefix of the same program, so bisecting `--length` finds the
   shortest program that still fails:
   ```sh
   make -C verif/random SEED_START=1234 SEEDS=1 LEN=300
   ```
   A failing run leaves the shorter `.S`, `.elf` and logs in
   `build/random/fail/`.
4. **Look at the waveform.** `build/sim/soc_sim --vcd t.vcd seed_1234.elf`
   dumps every signal. Count how often the failing pc retired before the
   mismatch (`head -n <line> seed_1234.rtl.commit | grep -c '^<pc>'`), find
   that occurrence of `rvfi_valid && rvfi_pc == <pc>` in the waveform, and
   walk back through the pipeline. `--max-cycles` keeps the dump short: the
   CPI from the summary line times the line number is a good bound.
5. **Decide who is wrong.** Check the instruction against the spec. If the
   ISS is wrong, fix it and add a unit test in `tools/iss/test/` with the
   value worked out by hand. Then rerun the seed and a full regression.

## 5. NPU unit tests

`verif/npu/` runs cocotb tests of `npu_top` on Verilator, for `ARR = 8` with
4-, 8- and 16-byte DMA ports and for `ARR = 4` with a 16-byte port
(`make test-npu`; see `docs/npu.md` section 7 for the full list). The
reference is `verif/npu/npu_model.py`, a Python transcription of the spec 5.1
pseudocode that shares no code with the RTL. The testbench checks the whole
DMEM image after every operation, plus `STATUS`, `irq`, `CYCLES` and the exact
DMA address sequence. Its DMEM model drives random data whenever no read
response is due, so a read sampled at the wrong time fails.

The ISS NPU model (`tools/iss/npu.cpp`) is a second, independent
implementation of the same pseudocode, in C++ with its own unit tests. The
firmware NPU tests (`npu_gemm`, `npu_ksplit`) run both end to end through the
driver: they compare every NPU result, over the whole canary-filled output
buffer, with a software GEMM and with literal known answers, and both
simulators must print the same pass counts.

## 6. Firmware tests

Every directory in `sw/tests/` is a C program built with the normal firmware
flow. `make test-sw` runs each one on the ISS and on the RTL and requires
exit code 0 and, when `tests/<name>/expected.txt` exists, UART output equal to
it byte for byte. This covers what the commit-log comparison cannot: programs
that poll the NPU or read `cycle`, and the full firmware stack (crt0, the
mini libc, the NPU driver). Firmware that does neither can also go through
`difftest.py` directly for an instruction-level comparison.

## 7. Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request, on Ubuntu
24.04, using the same make targets as a local checkout:

| Job | Runs |
|-----|------|
| `ISS (g++)`, `ISS (clang++)` | ISS build with `-Werror`, `make test-iss`, `make test-isa-iss`, a check that the riscv-tests refuse to run without their submodule, the firmware tests on the ISS (`make -C sw test SIMS=build/iss/iss`, the NPU tests also with other array sizes), `make -C sw npu-sweep`, 100 random difftest seeds (ISS on both sides) and `make -C verif/random selftest` |
| `NPU (lint, cocotb)` | `make lint` for every RTL top, `make test-npu` |
| `RTL (<test>)` | one job each, in parallel: `make test-isa-rtl`, the riscv-tests on `soc_sim_bp0`, `make test-core`, `bp-compare`, `randreset`, `uart-serial`, `make test-sw`, the firmware tests on `soc_sim_dma4` and on `soc_sim_dma8`, `make test-random`, and the random difftest on `soc_sim_bp0` |
| `Synthesis (Yosys, ECP5)` | `synth_ecp5` of `npu_top` and `soc_top` with the cell counts in the job summary, then the ULX3S place and route, timing report and bitstream |

The firmware toolchain is Ubuntu's `clang`, `lld` and `llvm` (clang 18 has
the RISC-V target, and `mk/toolchain.mk` uses the plain tool names when there
is no Homebrew LLVM). Verilator and Yosys come from a pinned YosysHQ OSS CAD
Suite release, because Ubuntu's Verilator is older than the 5.036 that cocotb
2.1 needs. The Python venv is cached on `requirements.txt`. A failing job
uploads its failing cases (`build/random/fail/`, test logs) as an artifact.
Each job installs only what it needs through `.github/actions/setup`, so a new
job is a checkout, one setup step with the tools it needs, and its make target.
