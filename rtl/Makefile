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
