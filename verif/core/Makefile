# Directed pipeline tests for rv32_core (docs/core.md, "Verification").
#
#   make          build the tests and both simulators, run every test on
#                 build/sim/soc_sim and build/sim/soc_sim_bp0, then `cli`
#   make build    only build the test ELFs into build/verif/core/
#   make cli      check that soc_sim accepts and rejects the same command
#                 lines and ELF files as the ISS (sim_cli.py)
#
# Each test is a bare-metal assembly program that checks its own results and
# reports through SIM_EXIT like the riscv-tests (0 = pass, (case << 1) | 1 =
# failure in that case). check.sh also compares the exit code with
# EXPECT_<test> (default 0), the UART output with <test>.out (default:
# nothing), and the commit log length with instret. timing.S is cycle-exact,
# so it is built once per predictor mode and run on the matching simulator.

include ../../mk/toolchain.mk

OUT := $(BUILD_DIR)/verif/core

TESTS := fwd loaduse muldiv csr bpred calls bus uart halt_ecall halt_ebreak halt_illegal \
         halt_fetch halt_fetch_end halt_fetch_misaligned

EXPECT_halt_ecall            := 2
EXPECT_halt_ebreak           := 2
EXPECT_halt_illegal          := 3
EXPECT_halt_fetch            := 3
EXPECT_halt_fetch_end        := 3
EXPECT_halt_fetch_misaligned := 3

SIM_BP1 := $(BUILD_DIR)/sim/soc_sim
SIM_BP0 := $(BUILD_DIR)/sim/soc_sim_bp0

TEST_CFLAGS  := $(RV_FLAGS) -I. -I$(REPO_ROOT)/sw/common
TEST_LDFLAGS := $(RV_LDFLAGS) -T$(REPO_ROOT)/sw/common/link.ld

ELFS := $(TESTS:%=$(OUT)/%.elf) $(OUT)/timing_bp1.elf $(OUT)/timing_bp0.elf

# test:expected_exit:expected_stdout, one word per run of a simulator.
specs = $(foreach t,$(TESTS),$(OUT)/$(t).elf:$(or $(EXPECT_$(t)),0):$(if $(wildcard $(t).out),$(t).out,-))

.PHONY: all build sims run cli clean
all: run cli

build: $(ELFS)

$(OUT)/%.elf: %.S test.h | $(OUT)
	$(RV_CC) $(TEST_CFLAGS) $(TEST_LDFLAGS) $< -o $@
	$(RV_OBJDUMP) -d $@ > $(@:.elf=.dis)

$(OUT)/timing_bp%.elf: timing.S test.h | $(OUT)
	$(RV_CC) $(TEST_CFLAGS) -DBP_MODE=$* $(TEST_LDFLAGS) $< -o $@
	$(RV_OBJDUMP) -d $@ > $(@:.elf=.dis)

$(OUT):
	mkdir -p $@

sims:
	$(MAKE) -C $(REPO_ROOT)/sim BP_MODE=1
	$(MAKE) -C $(REPO_ROOT)/sim BP_MODE=0
	$(MAKE) -C $(REPO_ROOT)/tools/iss

cli: build sims
	$(PYTHON) sim_cli.py $(OUT)/fwd.elf $(OUT)/sim_cli $(SIM_BP1) $(ISS)

run: build sims
	@pass=0; fail=0; \
	for sim in $(SIM_BP1) $(SIM_BP0); do \
	  case $$sim in *_bp0) timing=$(OUT)/timing_bp0.elf ;; *) timing=$(OUT)/timing_bp1.elf ;; esac; \
	  for spec in $(specs) $$timing:0:-; do \
	    elf=$${spec%%:*}; rest=$${spec#*:}; \
	    if sh ./check.sh $$sim $$elf $${rest%%:*} $${rest#*:} $(OUT); then pass=$$((pass+1)); \
	    else fail=$$((fail+1)); fi; \
	  done; \
	done; \
	echo "core directed tests: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ]

clean:
	rm -rf $(BUILD_DIR)/verif/core
