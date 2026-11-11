# Pulse SoC top-level build. Each target delegates to the directory that
# owns it; see docs/spec.md section 8 for the layout.

include mk/toolchain.mk

.PHONY: all sim iss sw riscv-tests lint venv \
        test test-iss test-isa test-isa-iss test-isa-rtl test-core test-variants \
        test-npu test-sw test-sw-arr test-random bench synth clean

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
# suites, and the real serial UART delivers the firmware output intact.
test-variants:
	$(MAKE) -C verif/core bp-compare randreset uart-serial

test-npu:
	$(MAKE) -C verif/npu

test-sw: sim iss sw
	$(MAKE) -C sw test

# The NPU driver reads ARR from the INFO register, so its tests must also
# pass on array sizes other than the built-in 8. The ISS takes the size as a
# flag; the tests check their own results, so the exit code decides.
NPU_TEST_ARRS ?= 4 5 6
test-sw-arr: iss sw
	@status=0; \
	for a in $(NPU_TEST_ARRS); do \
	  for t in npu_gemm npu_ksplit; do \
	    out=$(BUILD_DIR)/sw/$$t.iss_arr$$a.out; \
	    if $(ISS) --quiet --npu-arr $$a $(BUILD_DIR)/sw/$$t.elf > $$out; then echo "PASS $$t on iss --npu-arr $$a"; \
	    else echo "FAIL $$t on iss --npu-arr $$a (output in $$out)"; status=1; fi; \
	  done; \
	done; \
	exit $$status

test-random: sim iss
	$(MAKE) -C verif/random

test: lint test-iss test-isa test-core test-variants test-npu test-sw test-sw-arr test-random

bench: sim sw
	$(MAKE) -C sw bench

synth:
	$(MAKE) -C fpga synth

clean:
	rm -rf build
