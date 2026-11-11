# Builds every firmware program (each subdirectory of tests/ and bench/ that
# has a Makefile), runs the tests on the simulators and the benchmarks on one.
#
#   make                      build everything into build/sw/
#   make test [SIMS="..."]    run tests/ on each simulator (default: ISS and RTL);
#                             the NPU tests also on the ISS with NPU_ARRS sizes
#   make bench [SIM=...]      run bench/ on one simulator (default: RTL), write
#                             build/bench/results.{md,json}
#   make size                 IMEM/DMEM footprint of every program

include ../mk/toolchain.mk

PROGRAM_DIRS := $(patsubst %/Makefile,%,$(wildcard tests/*/Makefile bench/*/Makefile))
TEST_NAMES   := $(patsubst tests/%,%,$(filter tests/%,$(PROGRAM_DIRS)))
PROGRAMS     := $(notdir $(PROGRAM_DIRS))

# Simulator paths are usually given relative to the repository root
# (make -C sw test SIMS=build/iss/iss); one that exists relative to sw/ or
# is absolute is used as is.
sim_path = $(foreach s,$(1),$(if $(wildcard $(s)),$(abspath $(s)),$(REPO_ROOT)/$(s)))

.PHONY: all test bench size clean $(PROGRAM_DIRS)
all: $(PROGRAM_DIRS)

$(PROGRAM_DIRS):
	$(MAKE) -C $@

# Runs every test on each simulator in SIMS. A test passes when the program
# exits with 0 and, if tests/<name>/expected.txt exists, its UART output
# matches that file byte for byte.
#
# The NPU tests (npu_*) take ARR from the INFO register, so on the ISS they
# also run with --npu-arr set to each size in NPU_ARRS, which catches driver
# code that only works for the default 8 x 8 array. expected.txt holds the
# output for ARR = 8, so those extra runs are judged by the exit code alone.
SIMS ?= $(ISS) $(RTL_SIM)
NPU_ARRS ?= 2 3 4 5 6 7
test: all
	@status=0; \
	for d in $(TEST_NAMES); do \
	  elf=$(BUILD_DIR)/sw/$$d.elf; \
	  for sim in $(call sim_path,$(SIMS)); do \
	    name=$$(basename $$sim); out=$(BUILD_DIR)/sw/$$d.$$name.out; \
	    if $$sim --quiet $$elf > $$out; then \
	      if [ -f tests/$$d/expected.txt ] && ! cmp -s tests/$$d/expected.txt $$out; then \
	        echo "FAIL $$d on $$name (output differs from expected.txt)"; status=1; \
	      else echo "PASS $$d on $$name"; fi; \
	    else echo "FAIL $$d on $$name (nonzero exit)"; status=1; fi; \
	    case $$name:$$d in iss:npu_*) ;; *) continue ;; esac; \
	    for arr in $(NPU_ARRS); do \
	      out=$(BUILD_DIR)/sw/$$d.$$name-arr$$arr.out; \
	      if $$sim --quiet --npu-arr $$arr $$elf > $$out; then \
	        echo "PASS $$d on $$name --npu-arr $$arr"; \
	      else echo "FAIL $$d on $$name --npu-arr $$arr (nonzero exit)"; status=1; fi; \
	    done; \
	  done; \
	done; exit $$status

SIM ?= $(RTL_SIM)
bench: all
	$(PYTHON) $(REPO_ROOT)/tools/collect_bench.py --sim $(call sim_path,$(SIM)) \
	  --elf-dir $(BUILD_DIR)/sw --out-dir $(BUILD_DIR)/bench

# IMEM holds .text; DMEM holds .rodata, .data and .bss, plus the stack that
# the linker script reserves at the top (8 KiB unless STACK_SIZE is set).
size: all
	@printf '%-12s %10s %10s %16s\n' program "IMEM (B)" "DMEM (B)" "DMEM+stack (%)"
	@for p in $(PROGRAMS); do \
	  $(RV_SIZE) -A $(BUILD_DIR)/sw/$$p.elf | awk -v p=$$p ' \
	    $$1 ~ /^\.text/ { imem += $$2 } \
	    $$1 ~ /^\.(rodata|s?data|s?bss)/ { dmem += $$2 } \
	    END { printf "%-12s %10d %10d %15.1f%%\n", p, imem, dmem, 100 * (dmem + 8192) / 262144 }'; \
	done

clean:
	rm -rf ../build/sw
