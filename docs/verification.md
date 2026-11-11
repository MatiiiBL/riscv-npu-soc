# Pulse verification

Pulse is checked at several levels. Each level has its own reference, so a bug
has to slip past more than one independent check to reach the board:

| Level | What runs | Checked against | Location | Command |
|-------|-----------|-----------------|----------|---------|
| ISA compliance | riscv-tests `rv32ui` + `rv32um` (48 tests) | self-checking tests | `verif/riscv-tests/` | `make test-isa` |
| Core, directed | short pipeline-hazard programs | ISS + the cycle costs of spec section 6 | `verif/core/` | `make test-core` |
| Core, random | constrained-random RV32IM programs | ISS, commit log line by line | `verif/random/`, `tools/rvgen/`, `tools/difftest.py` | `make test-random` |
| NPU unit | cocotb tests of `npu_top` | Python golden model | `verif/npu/` | `make test-npu` |
| Firmware | C tests, including NPU driver tests | `expected.txt`, on both simulators | `sw/tests/` | `make test-sw` |
| NPU driver portability | the NPU firmware tests with `ARR` = 4, 5, 6 | the tests' own software reference, on the ISS | `sw/tests/npu_*` | `make test-sw-arr` |
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
`verif/core/` hit each one on purpose and also check the performance claims of
spec section 6, which a functional comparison cannot see: a load-use pair
costs exactly one bubble, a mispredicted branch two, a correctly predicted
taken branch none, and a divide stalls for the divider latency. Each program
runs through `difftest.py` against the ISS for its results, and through
`soc_sim --stats` for the performance counters (load-use stalls, mispredicts,
divider stall cycles) and the cycle count.

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
- **stays inside defined behavior.** Loads and stores are aligned and stay
  inside a 4 KiB scratch buffer in DMEM (the base register points at its
  middle, so offsets go both ways). Programs never read `cycle` or `time`,
  never touch the NPU, and never trap.
- **is deterministic.** A private splitmix64 PRNG makes the output depend only
  on `--seed` and `--length`, not on the Python version. The buffer contents
  come from a separate stream, so a shorter `--length` produces a prefix of the
  same program with the same data (see 4.4).
- **ends with `sw zero` to `SIM_EXIT`.** Both models exit 0, so only the
  comparison decides.

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
- forward branches of all six kinds on equal, zero, fresh or random operands;
  `JAL`, and `JALR` with offsets, with bit 0 set, with `rd == rs1`, and through
  a target loaded from memory;
- dependent chains of 2 to 5 ALU/M ops, writes to `x0`, `instret`/`instreth`/
  `minstret` reads, writes to CSRs that must be ignored, reads of CSRs that
  must return 0, `FENCE`/`FENCE.I`, and occasional UART writes (so stdout is
  compared too).

Over 40 seeds of 2000 instructions every branch type is both taken and not
taken thousands of times, backward loop branches are taken about 7000 times,
and there are about 12,000 load-use pairs (into ALU/address, branch, store
data and `JALR`). The base and counter registers are picked per seed, so no
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
result the way a broken RTL would: a wrong `rd_wdata`, store data or pc, a
dropped, duplicated or missing instruction, a changed UART byte, a different
exit code. `selftest.py` checks that difftest flags each fault at the right
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
   - `mem_addr`, `wmask`, `wdata`: store address, byte enables or store-data
     forwarding.
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
| `ISS (g++)`, `ISS (clang++)` | ISS build with `-Werror`, `make -C tools/iss test`, `make test-isa-iss`, the firmware tests on the ISS (`make -C sw test SIMS=build/iss/iss`), 100 random difftest seeds (ISS on both sides) and `make -C verif/random selftest` |
| `NPU (lint, cocotb)` | `make lint` for every RTL top in the tree, `make test-npu` |
| `RTL (<test>)` | `make test-isa-rtl`, `make -C verif/core`, `make test-sw` and `make test-random` against the Verilator SoC model; each is skipped with a notice while the files it needs (`sim/Makefile`, `verif/core/Makefile`) are absent |
| `Synthesis (Yosys, ECP5)` | `synth_ecp5` of `npu_top` (and of `soc_top` through `make -C rtl synth-check` once `rtl/soc/soc.f` exists), with the cell counts in the job summary |

The firmware toolchain is Ubuntu's `clang`, `lld` and `llvm` (clang 18 has
the RISC-V target, and `mk/toolchain.mk` uses the plain tool names when there
is no Homebrew LLVM). Verilator and Yosys come from a pinned YosysHQ OSS CAD
Suite release, because Ubuntu's Verilator is older than the 5.036 that cocotb
2.1 needs. The Python venv is cached on `requirements.txt`. A failing job
uploads its failing cases (`build/random/fail/`, test logs) as an artifact.
Each job installs only what it needs through `.github/actions/setup`, so a new
job is a checkout, one setup step with the tools it needs, and its make target.
