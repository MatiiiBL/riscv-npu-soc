# FPGA build for the Radiona ULX3S: Lattice LFE5U-85F, CABGA381, speed 6,
# 25 MHz oscillator. See docs/fpga.md.
#
#   make FW=hello        firmware images, synthesis, place and route and the
#                        bitstream build/fpga/pulse_hello.bit
#   make synth           synthesis and place and route, then the report (no
#                        bitstream); what the root `make synth` runs
#   make report          utilization and Fmax of the last place and route
#   make seeds           place and route once per seed in SEEDS, Fmax of each
#   make lint            Verilator lint of the board top
#   make prog            load the bitstream into the board's SRAM over USB
#
# FW names a program in sw/tests/ or sw/bench/ (default hello); its ELF is
# built by sw/ and becomes the block RAM contents of the bitstream. Every
# step writes to build/fpga/<FW>/, so builds of different programs coexist.
# CLK_MHZ (25, or a PLL setting of ulx3s_top) and NPU_DMA_BYTES (4, 8 or 16)
# are passed to the board top.
#
# Tools are taken from PATH, or from $(OSS_CAD_SUITE)/bin when OSS_CAD_SUITE
# names an unpacked YosysHQ OSS CAD Suite; each can also be set on its own
# (YOSYS, NEXTPNR, ECPPACK, OPENFPGALOADER, VERILATOR).

OSS_CAD_SUITE  ?=
TOOL_PREFIX    := $(if $(OSS_CAD_SUITE),$(OSS_CAD_SUITE)/bin/)
YOSYS          ?= $(TOOL_PREFIX)yosys
NEXTPNR        ?= $(TOOL_PREFIX)nextpnr-ecp5
ECPPACK        ?= $(TOOL_PREFIX)ecppack
OPENFPGALOADER ?= $(TOOL_PREFIX)openFPGALoader

include ../mk/toolchain.mk

FW            ?= hello
CLK_MHZ       ?= 40
NPU_DMA_BYTES ?= 16
SEED          ?= 1
SEEDS         ?= 1 2 3 4 5 6 7 8

SYNTH_FLAGS   ?=
NEXTPNR_FLAGS ?=
DEVICE        := --85k --package CABGA381 --speed 6

BOARD_DIR := ulx3s
LPF       := $(BOARD_DIR)/ulx3s_v20.lpf
LINT_VLT  := $(BOARD_DIR)/lint.vlt

OUT    := $(BUILD_DIR)/fpga
FW_OUT := $(OUT)/$(FW)
ELF    := $(BUILD_DIR)/sw/$(FW).elf
BIT    := $(OUT)/pulse_$(FW).bit
NETLIST := $(FW_OUT)/pulse.json
CONFIG  := $(FW_OUT)/pulse.config
REPORT  := $(FW_OUT)/report.json

# Expands a Verilator -F file list into its source paths, following nested
# -F lists (paths are relative to the list that names them). The result is
# normalized to paths from this directory (../rtl/core/rv32_core.sv, ...).
EXPAND_F = expand() { local d; d=$$(dirname $$1); while read -r a b; do \
             if [ "$$a" = "-F" ]; then expand "$$d/$$b"; elif [ -n "$$a" ]; then echo "$$d/$$a"; fi; \
           done < $$1; }
SOURCES := $(patsubst $(REPO_ROOT)/%,../%,$(abspath $(shell $(EXPAND_F); expand $(BOARD_DIR)/ulx3s.f)))

FW_DIR = $(firstword $(patsubst $(REPO_ROOT)/sw/%/Makefile,%,\
           $(wildcard $(REPO_ROOT)/sw/tests/$(FW)/Makefile $(REPO_ROOT)/sw/bench/$(FW)/Makefile)))

.PHONY: all synth report seeds lint prog clean FORCE
all: $(BIT)

RUN_LABEL = FW=$(FW), CLK_MHZ=$(CLK_MHZ), NPU_DMA_BYTES=$(NPU_DMA_BYTES), seed $(SEED)

synth: $(REPORT)
	@$(PYTHON) report.py --label "$(RUN_LABEL)" $(REPORT)

report: $(REPORT)
	@$(PYTHON) report.py --label "$(RUN_LABEL)" --paths 1 $(REPORT)

# A step reruns when its settings change, not only when its inputs do: each
# settings file is rewritten only if its content differs.
define settings
	@mkdir -p $(@D)
	@echo '$(1)' | cmp -s - $@ || echo '$(1)' > $@
endef

$(FW_OUT)/synth.settings: FORCE
	$(call settings,CLK_MHZ=$(CLK_MHZ) NPU_DMA_BYTES=$(NPU_DMA_BYTES) $(SYNTH_FLAGS))

$(FW_OUT)/pnr.settings: FORCE
	$(call settings,CLK_MHZ=$(CLK_MHZ) SEED=$(SEED) $(NEXTPNR_FLAGS))

# sw/ decides whether the ELF is out of date; the images are only rewritten
# when it was.
$(ELF): FORCE
	@test -n "$(FW_DIR)" || { echo "fpga: no program '$(FW)' in sw/tests or sw/bench" >&2; exit 1; }
	$(MAKE) -C $(REPO_ROOT)/sw $(FW_DIR)

# One rule writes all images (a pattern rule with several targets runs
# once): imem.hex, and one dmem<b>.hex per DMEM bank (NPU_DMA_BYTES / 4
# word-interleaved banks), which soc_top loads from the prefix DMEM_INIT.
$(OUT)/%/imem.hex $(OUT)/%/dmem0.hex: $(BUILD_DIR)/sw/%.elf $(OUT)/%/synth.settings
	@mkdir -p $(@D)
	$(PYTHON) $(REPO_ROOT)/tools/elf2hex.py $< --imem $(@D)/imem.hex \
	  --dmem $(@D)/dmem --dmem-banks $$(( $(NPU_DMA_BYTES) / 4 ))

$(NETLIST): $(SOURCES) $(FW_OUT)/imem.hex $(FW_OUT)/dmem0.hex $(FW_OUT)/synth.settings
	$(YOSYS) -q -l $(FW_OUT)/yosys.log -p "read_verilog -sv $(SOURCES); \
	  chparam -set CLK_MHZ $(CLK_MHZ) -set NPU_DMA_BYTES $(NPU_DMA_BYTES) \
	    -set IMEM_INIT \"$(FW_OUT)/imem.hex\" -set DMEM_INIT \"$(FW_OUT)/dmem\" ulx3s_top; \
	  synth_ecp5 $(SYNTH_FLAGS) -top ulx3s_top -json $@; \
	  tee -q -o $(FW_OUT)/stat.txt stat"

# The core clock's constraint comes from the LPF (25 MHz on clk_25mhz),
# carried through the PLL's dividers when there is one; --freq only covers
# clocks nextpnr cannot derive, and is CLK_MHZ so that it agrees. nextpnr
# exits nonzero when a clock misses its constraint, so a bitstream is only
# produced for a design that meets timing.
$(CONFIG): $(NETLIST) $(LPF) $(FW_OUT)/pnr.settings
	$(NEXTPNR) $(DEVICE) --json $< --lpf $(LPF) --freq $(CLK_MHZ) --seed $(SEED) \
	  --textcfg $@ --report $(REPORT) --log $(FW_OUT)/nextpnr.log -q $(NEXTPNR_FLAGS)

$(REPORT): $(CONFIG) ;

$(BIT): $(CONFIG)
	$(ECPPACK) --compress --input $< --bit $@
	@echo "fpga: $@"

# Seed sweep: the same netlist placed and routed with each seed, timing
# failures allowed, so the spread of Fmax is visible. Runs in parallel with -j.
SEED_REPORTS := $(foreach s,$(SEEDS),$(FW_OUT)/seed$(s)/report.json)

seeds: $(SEED_REPORTS)
	@$(PYTHON) report.py --label "FW=$(FW), CLK_MHZ=$(CLK_MHZ), NPU_DMA_BYTES=$(NPU_DMA_BYTES)" \
	  --seeds $(foreach s,$(SEEDS),$(s)=$(FW_OUT)/seed$(s)/report.json)

$(FW_OUT)/seed%/report.json: $(NETLIST) $(LPF)
	@mkdir -p $(@D)
	$(NEXTPNR) $(DEVICE) --json $< --lpf $(LPF) --freq $(CLK_MHZ) --seed $* \
	  --timing-allow-fail --report $@ --log $(@D)/nextpnr.log -q $(NEXTPNR_FLAGS)

# The ECP5 primitives (EHXPLLL) come from Yosys's own cell library.
ECP5_CELLS = $(shell $(YOSYS)-config --datdir)/ecp5/cells_bb.v

# Linted once with the oscillator as the clock and once through the PLL.
lint:
	@for mhz in 25 50; do \
	  $(VERILATOR) --lint-only -Wall $(REPO_ROOT)/rtl/lint.vlt $(LINT_VLT) $(ECP5_CELLS) \
	    -F $(BOARD_DIR)/ulx3s.f --top-module ulx3s_top -GCLK_MHZ=$$mhz || exit 1; \
	  echo "lint ulx3s_top (CLK_MHZ=$$mhz): clean"; \
	done

prog: $(BIT)
	$(OPENFPGALOADER) -b ulx3s $(BIT)

clean:
	rm -rf $(OUT)
