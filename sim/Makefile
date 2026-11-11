# Verilator model of soc_top plus the C++ harness (docs/spec.md 7).
#
#   make                    build/sim/soc_sim         BTB + 2-bit BHT, 16-byte NPU DMA port
#   make BP_MODE=0          build/sim/soc_sim_bp0     static not-taken prediction
#   make NPU_DMA_BYTES=4    build/sim/soc_sim_dma4    4- (or 8-) byte NPU DMA port
#
# The two knobs combine (BP_MODE=0 NPU_DMA_BYTES=4 builds soc_sim_bp0_dma4).
#
# Tracing is compiled in but only enabled at run time by --vcd, so normal
# runs pay almost nothing for it.

include ../mk/toolchain.mk

BP_MODE       ?= 1
NPU_DMA_BYTES ?= 16

SIM_DIR := $(BUILD_DIR)/sim
SUFFIX  := $(if $(filter 1,$(BP_MODE)),,_bp$(BP_MODE))$(if $(filter 16,$(NPU_DMA_BYTES)),,_dma$(NPU_DMA_BYTES))
SIM_BIN := $(SIM_DIR)/soc_sim$(SUFFIX)
OBJ_DIR := $(SIM_DIR)/obj_bp$(BP_MODE)_dma$(NPU_DMA_BYTES)

RTL_DIR  := $(REPO_ROOT)/rtl
RTL_SRCS := $(wildcard $(RTL_DIR)/*/*.sv $(RTL_DIR)/*/*.f)
CPP_SRCS := $(CURDIR)/soc_sim.cpp $(CURDIR)/elf_loader.cpp
CPP_DEPS := $(CPP_SRCS) $(CURDIR)/elf_loader.h

CXX_OPT := -O3

VFLAGS := --cc --exe --build -j 0 --trace \
          -O3 --x-assign fast --x-initial fast \
          --top-module soc_top -GBP_MODE=$(BP_MODE) -GNPU_DMA_BYTES=$(NPU_DMA_BYTES) -GSIM=1 \
          --Mdir $(OBJ_DIR) -o $(SIM_BIN) \
          -CFLAGS "$(CXX_OPT) -std=c++17" \
          -MAKEFLAGS "OPT_FAST=$(CXX_OPT) OPT_SLOW=$(CXX_OPT) OPT_GLOBAL=$(CXX_OPT)"

.PHONY: all clean
all: $(SIM_BIN)

$(SIM_BIN): $(RTL_SRCS) $(CPP_DEPS) sim.vlt Makefile
	@mkdir -p $(SIM_DIR)
	$(VERILATOR) $(VFLAGS) sim.vlt -F $(RTL_DIR)/soc/soc.f $(CPP_SRCS)

clean:
	rm -rf $(SIM_DIR)/obj_bp* $(SIM_DIR)/soc_sim $(SIM_DIR)/soc_sim_*
