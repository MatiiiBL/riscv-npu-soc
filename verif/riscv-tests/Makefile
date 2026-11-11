# Builds the official riscv-tests ISA suites (rv32ui, rv32um) against the
# Pulse bare-metal environment in env/, and runs them on a simulator.
#
#   make            build all test ELFs into build/riscv-tests/
#   make run-iss    run every test on the C++ ISS
#   make run-rtl    run every test on the Verilator RTL model
#
# Excluded tests:
#   fence_i  - self-modifying code; IMEM is not writable from the data port
#   ma_data  - misaligned accesses; the core does not support them (no traps)

include ../../mk/toolchain.mk

SUITE_DIR := riscv-tests/isa
OUT       := $(BUILD_DIR)/riscv-tests
EXCLUDE   := fence_i ma_data

RV32UI := $(filter-out $(EXCLUDE),$(basename $(notdir $(wildcard $(SUITE_DIR)/rv32ui/*.S))))
RV32UM := $(basename $(notdir $(wildcard $(SUITE_DIR)/rv32um/*.S)))

TESTS := $(addprefix rv32ui-p-,$(RV32UI)) $(addprefix rv32um-p-,$(RV32UM))
ELFS  := $(addprefix $(OUT)/,$(addsuffix .elf,$(TESTS)))

# The test list comes from the submodule. Without it the list is empty and
# every run would "pass" without running anything, so refuse to go on; a
# count other than 48 means the submodule or the exclusions changed.
EXPECTED_TESTS := 48
ifneq ($(MAKECMDGOALS),clean)
  ifeq ($(strip $(TESTS)),)
    $(error riscv-tests submodule missing: run git submodule update --init verif/riscv-tests/riscv-tests)
  endif
  ifneq ($(words $(TESTS)),$(EXPECTED_TESTS))
    $(error expected $(EXPECTED_TESTS) riscv-tests (rv32ui + rv32um without $(EXCLUDE)), found $(words $(TESTS)))
  endif
endif

TEST_CFLAGS := $(RV_FLAGS) -Ienv -I$(REPO_ROOT)/sw/common -I$(SUITE_DIR)/macros/scalar
TEST_LDFLAGS := $(RV_FLAGS) $(RV_LDFLAGS) -T$(REPO_ROOT)/sw/common/link.ld

.PHONY: all run-iss run-rtl list clean
all: $(ELFS)

$(OUT)/rv32ui-p-%.elf: $(SUITE_DIR)/rv32ui/%.S env/riscv_test.h | $(OUT)
	$(RV_CC) $(TEST_CFLAGS) $(TEST_LDFLAGS) $< -o $@
	$(RV_OBJDUMP) -d $@ > $(@:.elf=.dis)

$(OUT)/rv32um-p-%.elf: $(SUITE_DIR)/rv32um/%.S env/riscv_test.h | $(OUT)
	$(RV_CC) $(TEST_CFLAGS) $(TEST_LDFLAGS) $< -o $@
	$(RV_OBJDUMP) -d $@ > $(@:.elf=.dis)

$(OUT):
	mkdir -p $@

list:
	@echo $(TESTS)

# Runs each ELF on SIM (an executable taking the ELF path; exit code 0 = pass).
define run_suite
	@pass=0; fail=0; failed=""; \
	for t in $(TESTS); do \
	  if $(1) $(OUT)/$$t.elf > $(OUT)/$$t.$(2).log 2>&1; then pass=$$((pass+1)); \
	  else fail=$$((fail+1)); failed="$$failed $$t"; fi; \
	done; \
	echo "riscv-tests on $(2): $$pass passed, $$fail failed"; \
	if [ $$fail -ne 0 ]; then echo "FAILED:$$failed"; exit 1; fi
endef

run-iss: all
	$(call run_suite,$(ISS),iss)

run-rtl: all
	$(call run_suite,$(RTL_SIM),rtl)

clean:
	rm -rf $(OUT)
