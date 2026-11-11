# Verilator model of soc_top plus the C++ harness (docs/spec.md 7).
#
#   make              build/sim/soc_sim        BP_MODE=1: BTB + 2-bit BHT
#   make BP_MODE=0    build/sim/soc_sim_bp0    static not-taken
#
# Tracing is compiled in but only enabled at run time by --vcd, so normal
# runs pay almost nothing for it.

include ../mk/toolchain.mk

BP_MODE ?= 1

SIM_DIR := $(BUILD_DIR)/sim
SIM_BIN := $(SIM_DIR)/$(if $(filter 1,$(BP_MODE)),soc_sim,soc_sim_bp$(BP_MODE))
OBJ_DIR := $(SIM_DIR)/obj_bp$(BP_MODE)

RTL_DIR  := $(REPO_ROOT)/rtl
RTL_SRCS := $(wildcard $(RTL_DIR)/*/*.sv $(RTL_DIR)/*/*.f)
CPP_SRCS := $(CURDIR)/soc_sim.cpp $(CURDIR)/elf_loader.cpp
CPP_DEPS := $(CPP_SRCS) $(CURDIR)/elf_loader.h

CXX_OPT := -O3

VFLAGS := --cc --exe --build -j 0 --trace \
          -O3 --x-assign fast --x-initial fast \
          --top-module soc_top -GBP_MODE=$(BP_MODE) -GSIM=1 \
          --Mdir $(OBJ_DIR) -o $(SIM_BIN) \
          -CFLAGS "$(CXX_OPT) -std=c++17" \
          -MAKEFLAGS "OPT_FAST=$(CXX_OPT) OPT_SLOW=$(CXX_OPT) OPT_GLOBAL=$(CXX_OPT)"

.PHONY: all clean
all: $(SIM_BIN)

$(SIM_BIN): $(RTL_SRCS) $(CPP_DEPS) sim.vlt Makefile
	@mkdir -p $(SIM_DIR)
	$(VERILATOR) $(VFLAGS) sim.vlt -F $(RTL_DIR)/soc/soc.f $(CPP_SRCS)

clean:
	rm -rf $(SIM_DIR)/obj_bp* $(SIM_DIR)/soc_sim $(SIM_DIR)/soc_sim_bp*
