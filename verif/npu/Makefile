# cocotb unit tests for npu_top on Verilator (see test_npu.py).
#
#   make                 build and run the suite for ARR=8 and ARR=4
#   make ARRS=8          only one configuration
#   make SEED=7 OPS=1000 another random seed / longer regression
#   make TEST=test_k_split
#
# Exits nonzero if any test fails. Build output goes to build/npu/.

include ../../mk/toolchain.mk

ARRS ?= 8 4
KMAX ?= 1024
SEED ?= 1
OPS  ?= 300
TEST ?=

RUN_ARGS := $(foreach a,$(ARRS),--arr $(a)) --kmax $(KMAX) --seed $(SEED) \
            --random-ops $(OPS) $(foreach t,$(TEST),--test $(t))

.PHONY: all test waves clean
all: test

test:
	$(PYTHON) run.py $(RUN_ARGS)

waves:
	$(PYTHON) run.py $(RUN_ARGS) --waves

clean:
	rm -rf $(BUILD_DIR)/npu
