# Verilator lint for each RTL top. Each block directory keeps a file list
# (core/core.f, npu/npu.f, soc/soc.f): one source path per line, relative to
# the .f file itself (read with Verilator -F). soc.f pulls in the others.

include ../mk/toolchain.mk

LINT_FLAGS := --lint-only -Wall
LINT_VLT   := $(wildcard lint.vlt)

.PHONY: lint lint-core lint-npu lint-soc
lint: lint-core lint-npu lint-soc

lint-core:
	@if [ -f core/core.f ]; then \
	  $(VERILATOR) $(LINT_FLAGS) $(LINT_VLT) -F core/core.f --top-module rv32_core && echo "lint core: clean"; fi

lint-npu:
	@if [ -f npu/npu.f ]; then \
	  $(VERILATOR) $(LINT_FLAGS) $(LINT_VLT) -F npu/npu.f --top-module npu_top && echo "lint npu: clean"; fi

lint-soc:
	@if [ -f soc/soc.f ]; then \
	  $(VERILATOR) $(LINT_FLAGS) $(LINT_VLT) -F soc/soc.f --top-module soc_top && echo "lint soc: clean"; fi

# Synthesis smoke test: Yosys synth_ecp5 of soc_top with the hello firmware
# as the IMEM/DMEM init images, then the cell statistics. An image is needed
# because an uninitialised instruction ROM is undefined, and Yosys would
# optimise away everything that depends on fetched instructions.
# NPU_DMA_BYTES selects the NPU port width (4, 8 or 16); DMEM then has
# NPU_DMA_BYTES / 4 banks, each loading its own image dmem<b>.hex.
SYNTH_DIR     := $(BUILD_DIR)/synth-check
SYNTH_ELF     ?= $(BUILD_DIR)/sw/hello.elf
NPU_DMA_BYTES ?= 16

# Expands a Verilator -F file list into plain paths, following nested -F.
EXPAND_F = expand() { local d; d=$$(dirname $$1); while read -r a b; do \
             if [ "$$a" = "-F" ]; then expand "$$d/$$b"; elif [ -n "$$a" ]; then echo "$$d/$$a"; fi; \
           done < $$1; }

.PHONY: synth-check
synth-check:
	$(MAKE) -C ../sw
	@mkdir -p $(SYNTH_DIR)
	$(PYTHON) ../tools/elf2hex.py $(SYNTH_ELF) --imem $(SYNTH_DIR)/imem.hex \
	  --dmem $(SYNTH_DIR)/dmem --dmem-banks $$(( $(NPU_DMA_BYTES) / 4 ))
	@$(EXPAND_F); \
	$(YOSYS) -q -l $(SYNTH_DIR)/yosys.log -p "read_verilog -sv $$(expand soc/soc.f | tr '\n' ' '); \
	  chparam -set IMEM_INIT \"$(SYNTH_DIR)/imem.hex\" -set DMEM_INIT \"$(SYNTH_DIR)/dmem\" \
	    -set NPU_DMA_BYTES $(NPU_DMA_BYTES) soc_top; \
	  synth_ecp5 -top soc_top; tee -q -o $(SYNTH_DIR)/stat.txt stat"
	@grep -E '^ +[0-9]+ +(LUT4|TRELLIS_FF|TRELLIS_DPR16X4|CCU2C|DP16KD|PDPW16KD|MULT18X18D)$$' $(SYNTH_DIR)/stat.txt
