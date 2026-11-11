# Pulse SoC top-level build. Each target delegates to the directory that
# owns it; see docs/spec.md section 8 for the layout.

include mk/toolchain.mk

.PHONY: all sim iss sw riscv-tests lint venv \
        test test-iss test-isa test-isa-iss test-isa-rtl test-core test-variants \
        test-npu test-sw test-random bench synth clean

all: sim iss sw riscv-tests

sim:
	$(MAKE) -C sim

iss:
	$(MAKE) -C tools/iss

sw:
	$(MAKE) -C sw

riscv-tests:
	$(MAKE) -C verif/riscv-tests

lint:
	$(MAKE) -C rtl lint

# Python environment for cocotb tests, rvgen and the MNIST model.
venv:
	python3 -m venv .venv
	.venv/bin/pip install -q -r requirements.txt

test-iss:
	$(MAKE) -C tools/iss test

test-isa-iss: iss riscv-tests
	$(MAKE) -C verif/riscv-tests run-iss

test-isa-rtl: sim riscv-tests
	$(MAKE) -C verif/riscv-tests run-rtl

test-isa: test-isa-iss test-isa-rtl

# verif/core builds both predictor variants of the simulator itself.
test-core:
	$(MAKE) -C verif/core

# Simulator variants that must not change behavior: the two predictor builds
# give identical commit logs, a build that starts from random state passes the
# suites, the real serial UART delivers the firmware output intact, and the
# firmware tests pass with the narrower NPU DMA ports.
NPU_DMA_VARIANTS ?= 4 8
test-variants: sw
	$(MAKE) -C verif/core bp-compare randreset uart-serial
	@for b in $(NPU_DMA_VARIANTS); do \
	  $(MAKE) -C sim NPU_DMA_BYTES=$$b && \
	  $(MAKE) -C sw test SIMS=build/sim/soc_sim_dma$$b || exit 1; \
	done

test-npu:
	$(MAKE) -C verif/npu

test-sw: sim iss sw
	$(MAKE) -C sw test
	$(MAKE) -C sw npu-sweep

test-random: sim iss
	$(MAKE) -C verif/random

test: lint test-iss test-isa test-core test-variants test-npu test-sw test-random

bench: sim sw
	$(MAKE) -C sw bench

synth:
	$(MAKE) -C fpga synth

clean:
	rm -rf build
