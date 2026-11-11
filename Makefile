# Pulse SoC top-level build. Each target delegates to the directory that
# owns it; see docs/spec.md section 8 for the layout.

include mk/toolchain.mk

.PHONY: all sim iss sw riscv-tests lint venv \
        test test-isa test-isa-iss test-isa-rtl test-npu test-sw test-random \
        bench synth clean

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

test-isa-iss: iss riscv-tests
	$(MAKE) -C verif/riscv-tests run-iss

test-isa-rtl: sim riscv-tests
	$(MAKE) -C verif/riscv-tests run-rtl

test-isa: test-isa-iss test-isa-rtl

test-npu:
	$(MAKE) -C verif/npu

test-sw: sim iss sw
	$(MAKE) -C sw test

test-random: sim iss
	$(MAKE) -C verif/random

test: lint test-isa test-npu test-sw test-random

bench: sim sw
	$(MAKE) -C sw bench

synth:
	$(MAKE) -C fpga synth

clean:
	rm -rf build
