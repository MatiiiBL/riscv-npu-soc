# cocotb unit tests for npu_top on Verilator (see test_npu.py).
#
#   make                 build and run every configuration (ARR:DMA_BYTES)
#   make CONFIGS=8:16    only one configuration
#   make SEED=7 OPS=1000 another random seed / longer regression
#   make TEST=test_k_split
#
# Exits nonzero if any test fails. Build output goes to build/npu/.

include ../../mk/toolchain.mk

CONFIGS ?= 8:4 8:8 8:16 4:16
KMAX ?= 1024
SEED ?= 1
OPS  ?= 300
TEST ?=

RUN_ARGS := $(foreach c,$(CONFIGS),--config $(c)) --kmax $(KMAX) --seed $(SEED) \
            --random-ops $(OPS) $(foreach t,$(TEST),--test $(t))

.PHONY: all test waves clean
all: test

test:
	$(PYTHON) run.py $(RUN_ARGS)

waves:
	$(PYTHON) run.py $(RUN_ARGS) --waves

clean:
	rm -rf $(BUILD_DIR)/npu
