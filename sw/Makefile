# Builds every firmware program (each subdirectory of tests/ and bench/ that
# has a Makefile), runs the tests on the simulators and the benchmarks on one.
#
#   make                      build everything into build/sw/
#   make test [SIMS="..."]    run tests/ on each simulator (default: ISS and RTL);
#                             the NPU tests also on the ISS with NPU_ARRS sizes
#   make npu-sweep            the NPU tests on the ISS for every ARR x KMAX in
#                             SWEEP_ARRS x SWEEP_KMAXS
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

.PHONY: all test npu-sweep bench size clean $(PROGRAM_DIRS)
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

# Runs every NPU test on the ISS for each array size in SWEEP_ARRS crossed
# with each KMAX in SWEEP_KMAXS. A test that does not support a size says so
# and exits with EXIT_SKIP (77, sw/common/soc.h). SWEEP_SKIPS lists, as
# shell patterns over <test>:<arr>:<kmax>, the sizes each test documents
# that it skips; a skip anywhere else fails the sweep, like any other
# nonzero exit, so a test cannot quietly stop covering a size. Outputs go
# to build/sw/sweep/.
SWEEP_ARRS  ?= 1 2 3 4 5 6 7 8
SWEEP_KMAXS ?= 3 4 16 64 65 1024
SWEEP_SKIPS := npu_gemm:*:[1-3] | npu_oob:1:* | npu_oob:*:[1-7] | npu_overlap:1:* | npu_overlap:*:[1-3]
npu-sweep: all
	@iss=$(call sim_path,$(ISS)); dir=$(BUILD_DIR)/sw/sweep; mkdir -p $$dir; \
	pass=0; skip=0; fail=0; \
	for d in $(filter npu_%,$(TEST_NAMES)); do \
	  for arr in $(SWEEP_ARRS); do \
	    for kmax in $(SWEEP_KMAXS); do \
	      out=$$dir/$$d.arr$$arr.kmax$$kmax.out; \
	      $$iss --quiet --npu-arr $$arr --npu-kmax $$kmax $(BUILD_DIR)/sw/$$d.elf > $$out; rc=$$?; \
	      case $$d:$$arr:$$kmax in $(SWEEP_SKIPS)) may_skip=1 ;; *) may_skip=0 ;; esac; \
	      if [ $$rc -eq 0 ]; then pass=$$((pass + 1)); \
	      elif [ $$rc -eq 77 ] && [ $$may_skip -eq 1 ]; then skip=$$((skip + 1)); \
	      elif [ $$rc -eq 77 ]; then \
	        echo "FAIL $$d ARR=$$arr KMAX=$$kmax: skipped a size it should run (see $$out)"; \
	        fail=$$((fail + 1)); \
	      else echo "FAIL $$d ARR=$$arr KMAX=$$kmax: exit $$rc (see $$out)"; fail=$$((fail + 1)); fi; \
	    done; \
	  done; \
	done; \
	echo "npu-sweep: $$pass passed, $$skip skipped (unsupported sizes), $$fail failed"; \
	[ $$fail -eq 0 ]

SIM ?= $(RTL_SIM)
bench: all
	$(PYTHON) $(REPO_ROOT)/tools/collect_bench.py --sim $(call sim_path,$(SIM)) \
	  --elf-dir $(BUILD_DIR)/sw --out-dir $(BUILD_DIR)/bench

# IMEM holds .text; DMEM holds .rodata, .data and .bss, plus the stack that
# the linker script reserves. The stack size is read from each ELF's
# STACK_SIZE symbol (set by link.ld, 8 KiB unless a program overrides it).
RV_NM      ?= $(LLVM_PREFIX)llvm-nm
DMEM_BYTES := 262144
size: all
	@printf '%-14s %10s %10s %10s %16s\n' program "IMEM (B)" "DMEM (B)" "stack (B)" "DMEM+stack (%)"
	@for p in $(PROGRAMS); do \
	  elf=$(BUILD_DIR)/sw/$$p.elf; \
	  stack=$$($(RV_NM) -t d $$elf | awk '$$3 == "STACK_SIZE" { print $$1 + 0 }'); \
	  $(RV_SIZE) -A $$elf | awk -v p=$$p -v stack=$$stack ' \
	    $$1 ~ /^\.text/ { imem += $$2 } \
	    $$1 ~ /^\.(rodata|s?data|s?bss)/ { dmem += $$2 } \
	    END { printf "%-14s %10d %10d %10d %15.1f%%\n", p, imem, dmem, stack, \
	          100 * (dmem + stack) / $(DMEM_BYTES) }'; \
	done

# Everything this Makefile writes lives under BUILD_DIR.
clean:
	$(if $(strip $(BUILD_DIR)),,$(error BUILD_DIR is empty))
	rm -rf $(BUILD_DIR)/sw $(BUILD_DIR)/bench
