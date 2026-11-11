# Pulse on the ULX3S: FPGA build, timing and resources

`fpga/` takes `soc_top` (the RV32IM core and the NPU, [core.md](core.md) and
[npu.md](npu.md)) through an open-source flow to a bitstream for the Radiona
ULX3S: Yosys synthesis, nextpnr-ecp5 place and route, and ecppack. The
firmware is part of the bitstream: its IMEM and DMEM images initialize the
block RAMs, so the core runs it from reset as soon as configuration ends.

| File | Contents |
|------|----------|
| `fpga/ulx3s/ulx3s_top.sv` | board top: clock, reset, pins, `soc_top` with `SIM = 0` and the chosen `NPU_DMA_BYTES` |
| `fpga/ulx3s/ulx3s_v20.lpf` | pin constraints, the used lines of the board's official file |
| `fpga/ulx3s/ulx3s.f` | file list (`soc.f` and the board top) |
| `fpga/ulx3s/lint.vlt` | Verilator waivers for the board top |
| `fpga/Makefile` | images, synthesis, place and route, bitstream, report, seed sweep |
| `fpga/report.py` | utilization and timing tables from nextpnr's JSON report |

## 1. Board

| | |
|---|---|
| Board | Radiona ULX3S v2.x / v3.0.x |
| FPGA | Lattice LFE5U-85F, speed grade 6, CABGA381 (`--85k --package CABGA381 --speed 6`) |
| Resources | 83640 LUT4 and flip-flops, 208 DP16KD (18 Kbit) block RAMs, 156 MULT18X18D |
| Clock | 25 MHz oscillator on pin G2 |
| Host link | FTDI FT231X USB serial (also the JTAG programmer) |
| User I/O used | 8 LEDs, the F1 button |

## 2. Clocking

The default core clock is the **25 MHz oscillator itself** (`CLK_MHZ = 25`):
section 6 shows the design closes timing there with at least 5% margin on
every seed tried, and not at 30 MHz on any. Without a PLL there is no lock
time and no PLL jitter to budget for.

Any other `CLK_MHZ` instantiates an `EHXPLLL`. The board top keeps a table of
exact settings for 30, 35, 40, 45 and 50 MHz, copied from
`ecppll -i 25 -o <MHz>` (`CLKI_DIV`, `CLKFB_DIV`, `CLKOP_DIV`,
`CLKOP_CPHASE`, feedback from `CLKOP`, VCO between 400 and 800 MHz); a value
without an entry stops elaboration with an `$error`. These are there for
when the RTL changes in section 8 land: `make -C fpga CLK_MHZ=40` then builds
a 40 MHz design, and the build fails if it does not meet timing.

nextpnr gets the clock constraint from the LPF (`FREQUENCY PORT "clk_25mhz"
25 MHZ`) and, through a PLL, derives the output's constraint from the
dividers (`Derived frequency constraint of 30.0 MHz for net clk` in the
log). `--freq` is passed the same `CLK_MHZ` and only covers clocks nextpnr
cannot derive.

The UART divider follows the clock: `UART_DIV = round(CLK_MHZ * 10^6 /
115200)`, 217 at 25 MHz (115207 baud, +0.006%).

## 3. Reset

```
btn_fire1 --> 2 FF sync --+
                          +--> clear --> 17-bit counter --> MSB = rst_n
LOCK      --> 2 FF sync --+    (counts up to 2^16, then holds)
```

The F1 button (high while pressed) and the PLL's `LOCK` are asynchronous to
the core clock, so each goes through a two-flip-flop synchronizer. While
either asks for reset (button pressed, PLL unlocked) the counter is held at
zero; `rst_n` is its top bit, so the SoC leaves reset 65536 cycles (2.6 ms
at 25 MHz) after the last request. That waits for the PLL to settle and
absorbs button bounce: every bounce restarts the count. The counter and the
synchronizers are declared with initial values, which an ECP5 loads into
its flip-flops at the end of configuration, so the same counter is the
**power-on reset**: the SoC starts in reset and comes out of it 2.6 ms after
the bitstream is loaded. `soc_top`'s reset is synchronous (spec section 9),
and one flip-flop drives it.

## 4. Pin map

From the official constraint file
([emard/ulx3s](https://github.com/emard/ulx3s) `doc/constraints/ulx3s_v20.lpf`),
trimmed to the pins used; port names are the file's, except `btn_fire1`
(`btn[1]` there).

| Port | Pin | Direction | Use |
|------|-----|-----------|-----|
| `clk_25mhz` | G2 | in | 25 MHz oscillator |
| `btn_fire1` | R1 | in, pull-down | F1 button: reset |
| `ftdi_rxd` | L4 | out | UART TX of `soc_top` into the FTDI chip's RX (115200 8N1) |
| `led[0]` .. `led[7]` | B2, C2, C1, D2, D1, E2, E1, H3 | out | `LEDS` register, bits 0-7 |
| `wifi_gpio0` | L2 | out | held high, so the ESP32 boots its firmware rather than its serial bootloader |

All are LVCMOS33. The LPF also keeps the board's `SYSCONFIG` line; nextpnr
applies its compressed bitstream, 3.3 V configuration banks and 62 MHz
flash clock.

## 5. Building and programming

### Tools

nextpnr-ecp5 and ecppack are not in Homebrew; they come with the YosysHQ
[OSS CAD Suite](https://github.com/YosysHQ/oss-cad-suite-build/releases).
Unpack a release anywhere (CI and the numbers below use 2026-11-01: Yosys
0.70+286, nextpnr 0.11.1-85) and either put its `bin/` on `PATH` or point
`OSS_CAD_SUITE` at it:

```sh
tar -xzf oss-cad-suite-darwin-arm64-20261101.tgz -C ~/.local
export OSS_CAD_SUITE=~/.local/oss-cad-suite
```

The Makefile finds every tool through a variable (`YOSYS ?= yosys`,
`NEXTPNR ?= nextpnr-ecp5`, `ECPPACK ?= ecppack`,
`OPENFPGALOADER ?= openFPGALoader`, each prefixed with
`$(OSS_CAD_SUITE)/bin/` when that is set), so no path is built in. The
firmware toolchain is the usual one (`mk/toolchain.mk`).

### Commands

| Command | Does |
|---------|------|
| `make -C fpga FW=hello` | images, synthesis, place and route, `build/fpga/pulse_hello.bit` (2-3 minutes on an M4) |
| `make -C fpga report` | utilization, Fmax per clock and the critical path of that build |
| `make synth` (root), `make -C fpga synth` | synthesis and place and route plus the report, no bitstream |
| `make -C fpga seeds -j8` | place and route the same netlist once per seed in `SEEDS` (default 1-8), Fmax table |
| `make -C fpga lint` | Verilator `-Wall` lint of the board top, with and without the PLL |
| `make -C fpga prog` | `openFPGALoader -b ulx3s build/fpga/pulse_<FW>.bit` |

`FW` is any program in `sw/tests/` or `sw/bench/` (default `hello`); its ELF
is built by `sw/`. Other knobs: `CLK_MHZ` (25, or a PLL entry),
`NPU_DMA_BYTES` (4, 8 or 16, default 16), `SEED` (default 1), `SYNTH_FLAGS`,
`NEXTPNR_FLAGS`. Each program gets its own directory, `build/fpga/<FW>/`,
with the images (`imem.hex`, and `dmem0.hex` to `dmem<N-1>.hex` for the
`N = NPU_DMA_BYTES / 4` DMEM banks, which `soc_top` loads from the prefix
`DMEM_INIT`), the netlist (`pulse.json`), the Yosys and nextpnr logs,
nextpnr's textual configuration and its JSON report; a change of
`CLK_MHZ`, `NPU_DMA_BYTES`, `SEED` or the flags reruns the steps it
affects. Place and route uses a fixed seed, so a build is reproducible on
one machine; across platforms nextpnr's result can differ a little (seed 1
gives 27.03 MHz on macOS arm64 and 27.04 MHz in CI on Linux x86-64; during
bring-up an earlier netlist gave 27.65 and 28.43 MHz).

### Programming the board

Connect the ULX3S's US1 USB port (the FTDI chip). To load the bitstream
into the FPGA's configuration SRAM (lost at power-off):

```sh
openFPGALoader -b ulx3s build/fpga/pulse_hello.bit
```

To write it to the SPI flash instead, so the board boots it at power-up:

```sh
openFPGALoader -b ulx3s -f build/fpga/pulse_hello.bit
```

The same USB port is the serial console. Open it at 115200 8N1 (for
example `picocom -b 115200 /dev/ttyUSB0` on Linux or
`screen /dev/tty.usbserial-* 115200` on macOS) and press F1:

```
Hello from Pulse!
sum of squares 1..100 = 338350
```

On the board `SIM_EXIT` has no effect: after `main` returns, `crt0` writes
the exit code there and then spins, so a program prints once per reset.
Firmware that wants a visible result on its own can write it to `LEDS`.
These commands are documented, not tested here: no board was attached
while this flow was written, so "done" meant a bitstream that builds and
meets timing, from a design that boots from block RAM.

## 6. Results

Measured with OSS CAD Suite 2026-11-01 on macOS arm64, `FW=hello`,
`CLK_MHZ=25`, `NPU_DMA_BYTES=16` (DMEM in four word-interleaved banks, the
NPU's 16-byte DMA port).

### Synthesis choices

In this Yosys, `synth_ecp5` is a wrapper for `synth_lattice`, which always
maps logic with ABC9 (`abc9 -W 300`); the old `-abc9` flag is accepted and
changes nothing (the netlists are byte-identical). The flow therefore runs
plain `synth_ecp5`. The one ABC9 variant left, `-dff` (sequential mapping
across flip-flops), was tried during bring-up, on a board top that still
had a 25 MHz PLL: 150 s of synthesis instead of 13 s, 64 more LUT4 and 35
fewer flip-flops, and no better timing (26.6-28.1 MHz over seeds 1-4 at a
30 MHz target, against 27.2-28.5 MHz without it), so it is not used.

### Utilization

nextpnr's count after packing, `make -C fpga report`:

| Resource | Used | LFE5U-85F | Share |
|----------|-----:|----------:|------:|
| LUT4 (`TRELLIS_COMB`, carry halves included) | 14772 | 83640 | 17.7 % |
| Flip-flops (`TRELLIS_FF`) | 6913 | 83640 | 8.3 % |
| DP16KD block RAM | 175 | 208 | 84.1 % |
| MULT18X18D | 70 | 156 | 44.9 % |
| Distributed RAM (`TRELLIS_RAMW`) | 36 | 10455 | 0.3 % |
| I/O | 12 | 365 | 3.3 % |

The block RAMs are 128 for DMEM (four banks of 16K words, each 32 columns
of 16K x 1), 29 for IMEM (64 KiB; Yosys drops the 3 bit columns that are
constant in the `hello` image, so a program that uses every bit needs 32),
16 for the NPU's operand buffers and 2 for the BTB. That leaves 33 of the
208 free. The multipliers are 64 in the NPU's processing elements, 2 in
its requantization stage and 4 in the core's 33 x 33 multiplier. Yosys's
own count is 10708 LUT4, 1672 CCU2C carry cells and 6913 flip-flops;
nextpnr's `TRELLIS_COMB` also counts both halves of every CCU2C and the
LUTs it adds while packing. The board top drops the SoC's simulation taps,
retirement port and 64-bit performance counters, which is why it has fewer
flip-flops than `make -C rtl synth-check` reports for `soc_top`.

Before DMEM was banked (one 32-bit NPU port) the same flow gave 14231
LUT4, 167 DP16KD and 27.10-29.18 MHz over the same eight seeds: the 16-byte
port costs about 540 LUT4 and 8 block RAMs (the NPU's wider operand
buffers) and moves Fmax by less than the seed spread.

### Timing

Fmax per seed, the same netlist placed and routed at the 25 MHz constraint
(`make -C fpga seeds -j8`):

| Seed | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|------|---:|---:|---:|---:|---:|---:|---:|---:|
| Fmax (MHz) | 27.03 | 26.41 | 27.38 | 26.32 | 26.39 | 26.52 | 27.79 | 27.48 |

Minimum 26.32, median 26.77, maximum 27.79 MHz: a spread of 5.6%, every
seed meeting 25 MHz with 2.0 to 4.0 ns of slack. Asking for more does not
help: with the PLL at 30 MHz (`CLK_MHZ=30`) the timing-driven placer has a
tighter target but the same logic, and seeds 1-4 reach 28.34, 27.51, 28.44
and 27.92 MHz. None meets 30 MHz.

**Chosen clock: 25 MHz**, the oscillator, with at least 5.3% margin on all
eight seeds (nextpnr's delays are already its worst-case model for speed
grade 6). The build uses seed 1: 27.03 MHz, 3.0 ns of slack.

## 7. Critical path

nextpnr's critical path for seed 1 (`make -C fpga report`, full listing in
`build/fpga/hello/nextpnr.log`) is 37.0 ns: 15.6 ns in cells and 21.4 ns in
routing. It is a load forwarded into a multiply:

| ns | Cumulative | Where | RTL |
|---:|---:|-------|-----|
| 5.8 | 5.8 | clock-to-out of a DMEM block RAM, CPU port (`u_soc.u_dmem.g_bank[1].u_ram.mem.0.8`, output unregistered) | `rtl/soc/bram_tdp.sv:43` |
| 11.7 | 17.5 | 6.3 ns of routing out of the RAM array, then seven LUT levels and the wires between them, which ABC built from the 4:1 bank select, the read steering, the load byte select and sign extension, the WB result select, the forwarding mux and the multiplier operand's sign extension | `rtl/soc/soc_dmem.sv:86-91`, `rtl/soc/soc_bus.sv:117`, `rtl/core/rv32_core.sv:495-513`, `rtl/core/rv32_core.sv:286-294`, `rtl/core/rv32_muldiv.sv:40-41` |
| 3.7 | 21.2 | routing to a MULT18X18D | |
| 3.9 | 25.1 | the MULT18X18D | `rtl/core/rv32_muldiv.sv:42` |
| 10.3 | 35.4 | the carry chains that add the four 18 x 18 partial products, and the routing between them | `rtl/core/rv32_muldiv.sv:42` |
| 1.6 | 37.0 | `mul_y` select, result mux, setup at `res_m` | `rtl/core/rv32_core.sv:389-396, 416` |

The start is the read data of a load in WB, which comes straight from the
block RAM (DMEM has one cycle of read latency, spec section 4). A dependent
instruction one load-use bubble behind is in EX in that same cycle, and the
WB-to-EX forward hands it the data, so one cycle holds the RAM's 5.8 ns
clock-to-out, the routing out of a memory spread over 128 block RAMs, the
bank select and load alignment, and then a full 33 x 33 multiply with its
adder tree. Seven of the eight seeds have this path, from one bank or
another, as their critical path; seed 8's is the next one in section 8,
the same load feeding the branch redirect. The core already notes the
multiplier as the first thing to split if a faster clock is wanted
([core.md](core.md) section 3).

## 8. What would raise Fmax

The path above is the first of several that share its start. To see what
follows, the changes below were made to a scratch copy of the RTL, only to
measure timing (they are not functionally complete and are not in the
tree), synthesized with this board top and placed and routed with seeds 1
and 2 against a 40 or 45 MHz target:

| Change (timing experiment) | Fmax, seeds 1 / 2 | Critical path |
|----------------------------|------------------:|---------------|
| none (this tree, 25 MHz target) | 27.03 / 26.41 MHz | load in WB -> forward -> multiply -> `res_m` (section 7), 37.0-37.9 ns |
| multiplier operands registered (multiply from flip-flops, as it would be in MEM) | 27.74 / 27.61 MHz | load in WB -> forward -> ALU add for the JALR target (`rv32_alu.sv`) -> predicted-target compare -> `redirect_x` -> next PC -> BTB RAM address, 36.1-36.2 ns |
| and load data registered before it is forwarded | 36.95 / 38.17 MHz | IMEM block RAM -> decode -> load-use check -> `stall_d` -> `imem_req` -> IMEM output-register reset, or -> next PC -> BTB RAM address, 26.2-27.1 ns |

So the fix, in order:

1. **Take the multiply out of EX.** Register the operands at EX/MEM (the
   MULT18X18D input registers can hold them) and finish the product and the
   high/low select in MEM; a dependent instruction then waits one cycle,
   like after a load (a "mul-use" interlock next to `lu_hazard`,
   `rv32_core.sv:226`). On its own this gains only 3-4%, because the next
   path is the same load feeding the branch logic: the redirect
   (`rv32_core.sv:351-352`) drives the next PC, which Yosys feeds straight
   into the BTB RAMs' address registers.
2. **Forward load data from a register.** Add a pipeline register after
   the load alignment and forward from it, so a load costs two bubbles to
   a dependent instruction instead of one. Together with step 1 this gives
   37.0-38.2 MHz, about 1.4x. It changes spec section 6 ("a load followed
   by a dependent instruction costs exactly one bubble"), so it is a design
   decision, not just a fix. A cheaper variant keeps one bubble for ALU
   consumers and gives two only to branches, JALR and multiplies, the
   consumers whose paths are long.
3. **Then the fetch stall.** The load-use check (`rv32_core.sv:226-228`)
   is computed from the instruction word straight out of IMEM and, through
   `stall_d`, holds the PC and gates the IMEM read (`imem_req`,
   `rv32_core.sv:110`) in the same cycle; `imem_req` reaches the RAM's
   clock enable and, with the bad-fetch clear, its output reset. Decoding
   `uses_rs1`/`uses_rs2` from fewer bits, or holding a stalled instruction
   in a register instead of stopping the RAM, shortens it.

With steps 1 and 2, `CLK_MHZ=35` would leave a margin like today's (5-9%).
Whether that pays depends on the extra bubbles: the clock gains 40%, while
CoreMark's CPI of 1.17 rises by the share of loads followed immediately by
their consumer, which `soc_sim --stats` counts (`load-use stalls`).

The NPU is not on any of these paths. Placed and routed on its own, with
`DMA_BYTES = 16` and every port behind a flip-flop (inputs from a shift
register, outputs registered, since its 387 port bits do not fit the
package's 365 pins), it reaches 73.30 and 76.55 MHz with seeds 1 and 2, so
the SoC's clock is set by the core. Its critical path, 13.1-13.6 ns, ends at
the DMA's remaining-length register (`rem_q`, `rtl/npu/npu_dma.sv:161`),
which START loads from the operand address and `K` registers. That is the
START decode that [npu.md](npu.md) section 8 already lists as a stage to
pipeline if needed.

## 9. Continuous integration

The `Synthesis (Yosys, ECP5)` job in `.github/workflows/ci.yml` installs
the pinned OSS CAD Suite (which includes nextpnr-ecp5 and ecppack), reports
the cell counts of `npu_top` and `soc_top`, lints the board top, runs
`make -C fpga FW=hello`, writes `make -C fpga report` (utilization, Fmax,
critical path) to the job summary, and uploads `pulse_hello.bit` with the
nextpnr report and log as the `ulx3s-hello` artifact. A missed timing
constraint fails the job.

## 10. Limitations

- Untested on hardware: no board was attached. The bitstream builds, meets
  timing and holds the firmware in block RAM; the pin map, the reset and
  the programming commands follow the board's documentation.
- The firmware is fixed at build time. A new program means a new synthesis
  and place and route (2-3 minutes), since Yosys prunes IMEM bit columns
  that are constant in the image, which rules out patching block RAM
  contents in an existing bitstream.
- Only UART TX, the LEDs and one button are wired; the SDRAM, the other
  buttons and the GPIO headers are unused.
