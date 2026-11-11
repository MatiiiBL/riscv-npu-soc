# Random differential testing (docs/verification.md): generate programs with
# tools/rvgen, build them with the firmware toolchain, and check the RTL
# simulator against the ISS with tools/difftest.py. Use -j to run seeds in
# parallel.
#
#   make -j8                          seeds 1..200, 2000 instructions each
#   make SEED_START=5000 SEEDS=50     seeds 5000..5049
#   make RTL_SIM=build/iss/iss        ISS on both sides, to check the flow
#   make selftest                     check that difftest catches injected faults
#
# A failing seed keeps its .S, .elf, commit logs and difftest report in
# build/random/fail/. Rerun one with
#   python3 tools/difftest.py --rtl build/sim/soc_sim --iss build/iss/iss build/random/fail/seed_<N>.elf

include ../../mk/toolchain.mk

SEEDS      ?= 200
SEED_START ?= 1
LEN        ?= 2000
TIMEOUT    ?= 300

OUT  := $(BUILD_DIR)/random
WORK := $(OUT)/work
FAIL := $(OUT)/fail

# A relative simulator path may be given from the repository root (as in
# make -C verif/random RTL_SIM=build/iss/iss) or from this directory.
resolve = $(if $(filter /%,$(1)),$(1),$(if $(wildcard $(1)),$(abspath $(1)),$(REPO_ROOT)/$(1)))
RTL_BIN := $(call resolve,$(RTL_SIM))
ISS_BIN := $(call resolve,$(ISS))

RVGEN     := $(REPO_ROOT)/tools/rvgen/rvgen.py
DIFFTEST  := $(REPO_ROOT)/tools/difftest.py
GEN_FLAGS := $(RV_FLAGS) $(RV_LDFLAGS) -T$(REPO_ROOT)/sw/common/link.ld -I$(REPO_ROOT)/sw/common

SEED_LIST := $(shell seq $(SEED_START) $$(($(SEED_START) + $(SEEDS) - 1)))
RUNS      := $(addprefix run-,$(SEED_LIST))

.PHONY: all summary prepare selftest clean $(RUNS)
all: summary

prepare:
	@test -x $(RTL_BIN) || { echo "error: no RTL simulator at $(RTL_BIN) (make sim, or pass RTL_SIM=...)"; exit 1; }
	@test -x $(ISS_BIN) || { echo "error: no ISS at $(ISS_BIN) (make iss)"; exit 1; }
	@rm -rf $(WORK) $(FAIL)
	@mkdir -p $(WORK) $(FAIL)

# One seed: generate, build, difftest. The recipe never fails, so every seed
# runs; it records PASS or the failure reason for the summary, and keeps the
# files of failing seeds only.
$(RUNS): run-%: prepare
	@base=$(WORK)/seed_$*; \
	if ! $(PYTHON) $(RVGEN) --seed $* --length $(LEN) -o $$base.S > $$base.report 2>&1; then \
	  result="rvgen failed"; \
	elif ! $(RV_CC) $(GEN_FLAGS) $$base.S -o $$base.elf >> $$base.report 2>&1; then \
	  result="build failed"; \
	elif $(PYTHON) $(DIFFTEST) --rtl $(RTL_BIN) --iss $(ISS_BIN) --objdump $(RV_OBJDUMP) \
	       --work-dir $(WORK) --timeout $(TIMEOUT) --expect-exit 0 --quiet $$base.elf >> $$base.report 2>&1; then \
	  result=PASS; \
	else \
	  result=mismatch; \
	fi; \
	echo "$$result" > $$base.result; \
	if [ "$$result" = PASS ]; then \
	  rm -f $$base.S $$base.elf $$base.report $$base.rtl.* $$base.iss.*; \
	else \
	  cp $$base.* $(FAIL)/; \
	  first=$$(grep -m1 '^  ' $$base.report | sed 's/^ *//'); \
	  echo "FAIL seed $*: $$result: $$first (see $(FAIL)/seed_$*.report)"; \
	fi

summary: $(RUNS)
	@pass=0; fail=0; \
	for s in $(SEED_LIST); do \
	  if [ "$$(cat $(WORK)/seed_$$s.result)" = PASS ]; then pass=$$((pass + 1)); else fail=$$((fail + 1)); fi; \
	done; \
	echo "random difftest: $$pass passed, $$fail failed (seeds $(SEED_START)..$(lastword $(SEED_LIST)), LEN=$(LEN), RTL_SIM=$(RTL_BIN))"; \
	if [ $$fail -ne 0 ]; then echo "failing cases kept in $(FAIL)"; exit 1; fi

# Injects faults into an ISS run with fault_sim.py and checks that difftest
# reports each one, then prints an example mismatch report.
SELFTEST_SEED ?= 1
selftest:
	@test -x $(ISS_BIN) || { echo "error: no ISS at $(ISS_BIN) (make iss)"; exit 1; }
	@mkdir -p $(OUT)
	$(PYTHON) $(RVGEN) --seed $(SELFTEST_SEED) --length $(LEN) -o $(OUT)/selftest.S
	$(RV_CC) $(GEN_FLAGS) $(OUT)/selftest.S -o $(OUT)/selftest.elf
	$(PYTHON) selftest.py --iss $(ISS_BIN) --objdump $(RV_OBJDUMP) $(OUT)/selftest.elf

clean:
	rm -rf $(OUT)
