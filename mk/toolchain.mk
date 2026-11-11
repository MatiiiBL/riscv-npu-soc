# Shared toolchain settings. Included by every sub-Makefile.
#
# Firmware is built with LLVM (clang + lld), which ships a RISC-V backend on
# both macOS (Homebrew `llvm` + `lld`) and Ubuntu (`clang lld llvm`), so the
# same flags work locally and in CI without a GNU cross toolchain.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
BUILD_DIR ?= $(REPO_ROOT)/build

# Prefer Homebrew LLVM on macOS (Apple clang has no RISC-V target).
LLVM_BIN ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin))
ifneq ($(LLVM_BIN),)
  LLVM_PREFIX := $(LLVM_BIN)/
else
  LLVM_PREFIX :=
endif

RV_CC      ?= $(LLVM_PREFIX)clang
RV_OBJCOPY ?= $(LLVM_PREFIX)llvm-objcopy
RV_OBJDUMP ?= $(LLVM_PREFIX)llvm-objdump
RV_SIZE    ?= $(LLVM_PREFIX)llvm-size

RV_ARCH  := -march=rv32im_zicsr -mabi=ilp32
RV_FLAGS := --target=riscv32-unknown-elf $(RV_ARCH) -mcmodel=medany \
            -ffreestanding -fno-builtin \
            -fno-pic -fno-common -ffunction-sections -fdata-sections \
            -mno-relax
RV_LDFLAGS := -nostdlib -fuse-ld=lld -Wl,--gc-sections -Wl,--no-relax

VERILATOR ?= verilator
YOSYS     ?= yosys
PYTHON    ?= $(if $(wildcard $(REPO_ROOT)/.venv/bin/python),$(REPO_ROOT)/.venv/bin/python,python3)

# Simulators (built by sim/ and tools/iss/).
RTL_SIM := $(BUILD_DIR)/sim/soc_sim
ISS     := $(BUILD_DIR)/iss/iss
