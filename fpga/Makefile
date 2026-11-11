# FPGA build for the Radiona ULX3S: Lattice LFE5U-85F, CABGA381, speed 6,
# 25 MHz oscillator.
#
#   make lint            Verilator lint of the board top
#
# Tools are taken from PATH, or from $(OSS_CAD_SUITE)/bin when OSS_CAD_SUITE
# names an unpacked YosysHQ OSS CAD Suite; each can also be set on its own
# (YOSYS, VERILATOR).

OSS_CAD_SUITE  ?=
TOOL_PREFIX    := $(if $(OSS_CAD_SUITE),$(OSS_CAD_SUITE)/bin/)
YOSYS          ?= $(TOOL_PREFIX)yosys

include ../mk/toolchain.mk

BOARD_DIR := ulx3s
LINT_VLT  := $(BOARD_DIR)/lint.vlt

.PHONY: lint

# The ECP5 primitives (EHXPLLL) come from Yosys's own cell library.
ECP5_CELLS = $(shell $(YOSYS)-config --datdir)/ecp5/cells_bb.v

# Linted once with the oscillator as the clock and once through the PLL.
lint:
	@for mhz in 25 50; do \
	  $(VERILATOR) --lint-only -Wall $(REPO_ROOT)/rtl/lint.vlt $(LINT_VLT) $(ECP5_CELLS) \
	    -F $(BOARD_DIR)/ulx3s.f --top-module ulx3s_top -GCLK_MHZ=$$mhz || exit 1; \
	  echo "lint ulx3s_top (CLK_MHZ=$$mhz): clean"; \
	done
