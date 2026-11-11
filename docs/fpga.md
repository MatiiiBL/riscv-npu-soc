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

The core clock is **40 MHz from the PLL** (`CLK_MHZ = 40`, the default):
section 6 shows the design closes timing there with at least 5% margin on
every seed tried, and 45 MHz on only one of eight. The 25 MHz oscillator
feeds an `EHXPLLL` with `CLKI_DIV = 5`, `CLKFB_DIV = 8`, `CLKOP_DIV = 15`
(a 600 MHz VCO). The board top keeps a table of exact settings for 30, 35,
40, 45 and 50 MHz, copied from `ecppll -i 25 -o <MHz>` (`CLKI_DIV`,
`CLKFB_DIV`, `CLKOP_DIV`, `CLKOP_CPHASE`, feedback from `CLKOP`, VCO
between 400 and 800 MHz); a value without an entry stops elaboration with
an `$error`. `CLK_MHZ = 25` bypasses the PLL and clocks the SoC from the
oscillator directly, which is what the pipeline before the timing work of
[core.md](core.md) section 10 could run at.

nextpnr gets the clock constraint from the LPF (`FREQUENCY PORT "clk_25mhz"
25 MHZ`) and derives the PLL output's constraint from the dividers
(`Derived frequency constraint of 40.0 MHz for net clk` in the log).
`--freq` is passed the same `CLK_MHZ` and only covers clocks nextpnr
cannot derive.

The UART divider follows the clock: `UART_DIV = round(CLK_MHZ * 10^6 /
115200)`, 347 at 40 MHz (115274 baud, +0.06%).

## 3. Reset

```
btn_fire1 --> 2 FF sync --+
                          +--> clear --> 17-bit counter --> MSB = rst_n
LOCK      --> 2 FF sync --+    (counts up to 2^16, then holds)
```

The F1 button (high while pressed) and the PLL's `LOCK` are asynchronous to
the core clock, so each goes through a two-flip-flop synchronizer. While
either asks for reset (button pressed, PLL unlocked) the counter is held at
zero; `rst_n` is its top bit, so the SoC leaves reset 65536 cycles (1.6 ms
at 40 MHz) after the last request. That waits for the PLL to settle and
absorbs button bounce: every bounce restarts the count. The counter and the
synchronizers are declared with initial values, which an ECP5 loads into
its flip-flops at the end of configuration, so the same counter is the
**power-on reset**: the SoC starts in reset and comes out of it 1.6 ms after
the PLL locks. `soc_top`'s reset is synchronous (spec section 9),
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
is built by `sw/`. Other knobs: `CLK_MHZ` (default 40; 25, or a PLL entry),
`NPU_DMA_BYTES` (4, 8 or 16, default 16), `SEED` (default 1), `SYNTH_FLAGS`,
`NEXTPNR_FLAGS`. Each program gets its own directory, `build/fpga/<FW>/`,
with the images (`imem.hex`, and `dmem0.hex` to `dmem<N-1>.hex` for the
`N = NPU_DMA_BYTES / 4` DMEM banks, which `soc_top` loads from the prefix
`DMEM_INIT`), the netlist (`pulse.json`), the Yosys and nextpnr logs,
nextpnr's textual configuration and its JSON report; a change of
`CLK_MHZ`, `NPU_DMA_BYTES`, `SEED` or the flags reruns the steps it
affects. Place and route uses a fixed seed, so a build is reproducible on
one machine; across platforms nextpnr's result can differ a little (seed 1
gives 43.62 MHz on macOS arm64 and 42.68 MHz in CI on Linux x86-64, both
meeting 40 MHz).

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
`CLK_MHZ=40`, `NPU_DMA_BYTES=16` (DMEM in four word-interleaved banks, the
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
| LUT4 (`TRELLIS_COMB`, carry halves included) | 15098 | 83640 | 18.1 % |
| Flip-flops (`TRELLIS_FF`) | 7025 | 83640 | 8.4 % |
| DP16KD block RAM | 173 | 208 | 83.2 % |
| MULT18X18D | 70 | 156 | 44.9 % |
| Distributed RAM (`TRELLIS_RAMW`) | 92 | 10455 | 0.9 % |
| I/O | 12 | 365 | 3.3 % |
| PLL (`EHXPLLL`) | 1 | 4 | 25.0 % |

The block RAMs are 128 for DMEM (four banks of 16K words, each 32 columns
of 16K x 1), 29 for IMEM (64 KiB; Yosys drops the 3 bit columns that are
constant in the `hello` image, so a program that uses every bit needs 32)
and 16 for the NPU's operand buffers. That leaves 35 of the 208 free. The
distributed RAM is the register file (32 `DPR16X4`, 16 per read port) and
the BTB (60). The multipliers are 64 in the NPU's processing elements, 2 in
its requantization stage and 4 in the core's 33 x 33 multiplier. Yosys's
own count is 10704 LUT4, 1671 CCU2C carry cells and 7025 flip-flops;
nextpnr's `TRELLIS_COMB` also counts both halves of every CCU2C and the
LUTs it adds while packing. The board top drops the SoC's simulation taps,
retirement port and 64-bit performance counters, which is why it has fewer
flip-flops than `make -C rtl synth-check` reports for `soc_top`.

The 25 MHz pipeline that preceded the timing work of [core.md](core.md)
section 10 used 14772 LUT4, 6913 flip-flops, 175 DP16KD and 36 `DPR16X4`:
the 40 MHz one costs about 330 LUT4 and 110 flip-flops (the multiplier's
operand registers, the skid register, the interlock masks) and moves the
BTB from 2 block RAMs into LUT RAM.

### Timing

Fmax per seed, the same netlist placed and routed against the clock it runs
at (`make -C fpga seeds -j8`), before and after the timing work:

| Seed | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|------|---:|---:|---:|---:|---:|---:|---:|---:|
| 25 MHz pipeline, 25 MHz constraint | 27.03 | 26.41 | 27.38 | 26.32 | 26.39 | 26.52 | 27.79 | 27.48 |
| 40 MHz pipeline, 40 MHz constraint | 43.62 | 43.41 | 46.51 | 44.86 | 42.83 | 42.98 | 44.13 | 42.06 |

Now: minimum 42.06, median 43.51, maximum 46.51 MHz (before: 26.32 /
26.77 / 27.79), every seed meeting 40 MHz with 1.2 to 3.5 ns of slack. 45
MHz, the next PLL setting, would be met by one seed in eight.

**Chosen clock: 40 MHz**, from the PLL, with at least 5.1% margin on all
eight seeds (nextpnr's delays are already its worst-case model for speed
grade 6). The build uses seed 1: 43.62 MHz, 2.07 ns of slack.

The cycle cost of the faster pipeline (longer load-use and mul-use
interlocks) and what it buys in wall-clock time, from [core.md](core.md)
section 10:

| | 25 MHz pipeline | 40 MHz pipeline |
|---|---:|---:|
| CoreMark/MHz (`BP_MODE = 1`) | 3.291 | 2.855 |
| CoreMark/s | 82.3 | 114.2 (1.39x) |
| MNIST on the NPU, cycles per inference | 3027 | 3053 |
| MNIST on the NPU, us per inference | 121.1 | 76.3 (1.59x) |

## 7. Critical path

nextpnr's critical path for seed 1 (`make -C fpga report`, full listing in
`build/fpga/hello/nextpnr.log`) is 22.9 ns: 8.9 ns in cells and 14.0 ns in
routing. It runs from the instruction word to the stall that holds the
fetch PC:

| ns | Cumulative | Where | RTL |
|---:|---:|-------|-----|
| 5.6 | 5.6 | clock-to-out of an IMEM block RAM (the instruction word in ID) | `rtl/soc/bram_sp.sv:29` |
| 3.0 | 8.7 | routing out of the IMEM array | |
| 11.4 | 20.1 | ten LUT levels and the wires between them: the skid mux, the opcode decode of `uses_rs1`/`uses_rs2`, the lookup of `rs1`/`rs2` in the pending-register masks, the stall | `rtl/core/rv32_core.sv:192`, `rtl/core/rv32_decode.sv:52-56`, `rtl/core/rv32_core.sv:290-294` |
| 2.8 | 22.9 | a 4:1 mux (PFUMX and L6MUX) and its routing into a copy of `pc_f` that Yosys builds as the read-address register of the BTB LUT RAM; there the stall is a mux select, not a clock enable | `rtl/core/rv32_core.sv:144-145`, `rtl/core/rv32_bpred.sv:64` |

All eight seeds end on this cone, at that `pc_f` copy, at `pc_f` itself
(its clock enable) or at the ID/EX registers.

Before the timing work, the critical path was 37.0 ns: a load's data
straight out of a DMEM block RAM (5.8 ns clock-to-out, 6.3 ns of routing
out of the 128-RAM array, bank select and alignment), forwarded from WB
into the 33 x 33 multiply in EX, its two carry chains and the result mux
into `res_m`.

## 8. From 25 to 40 MHz

Every critical path of the 25 MHz pipeline started at a block RAM output.
[core.md](core.md) section 10 takes them off one at a time, each step
placed and routed with seeds 1-4 against a 50 MHz target:

| Step | Fmax, seeds 1-4 (MHz) |
|------|----------------------:|
| the 25 MHz pipeline (8 seeds, 25 MHz target) | 26.3-27.8 |
| multiply in MEM, one-cycle mul-use interlock | 27.3-28.3 |
| load data not forwarded (two bubbles for an adjacent consumer, one a step later) | 33.6-38.4 |
| IMEM reads every cycle, stalled instruction in a skid register | 36.0-40.0 |
| interlock operand flags from the opcode alone | 36.9-39.5 |
| BTB in LUT RAM, predicted-target compare in EX | 40.1-44.0 |
| interlock as a lookup in one-hot masks of pending registers | 43.7-46.3 |

A variant that keeps one bubble for ALU consumers of a load was also built:
its block RAM -> ALU -> `res_m` path stops it at 37.4-38.9 MHz, so it was
not kept. Going further than 40 MHz would take a decoupled fetch stage (an
instruction queue, so that the stall no longer holds the fetch PC), and
then the multiply in MEM, about 20 ns, would need a second stage. Both are
larger changes for at most 10-15% more clock.

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
