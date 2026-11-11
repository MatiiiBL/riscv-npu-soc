# Builds every firmware program (each subdirectory of tests/ and bench/ that
# has a Makefile) and runs the tests on the simulators.

include ../mk/toolchain.mk

PROGRAM_DIRS := $(patsubst %/Makefile,%,$(wildcard tests/*/Makefile bench/*/Makefile))
TEST_NAMES   := $(patsubst tests/%,%,$(filter tests/%,$(PROGRAM_DIRS)))

.PHONY: all test bench clean $(PROGRAM_DIRS)
all: $(PROGRAM_DIRS)

$(PROGRAM_DIRS):
	$(MAKE) -C $@

# Runs every test on each simulator in SIMS. A test passes when the program
# exits with 0 and, if tests/<name>/expected.txt exists, its UART output
# matches that file byte for byte.
SIMS ?= $(ISS) $(RTL_SIM)
test: all
	@status=0; \
	for d in $(TEST_NAMES); do \
	  elf=$(BUILD_DIR)/sw/$$d.elf; \
	  for sim in $(SIMS); do \
	    name=$$(basename $$sim); out=$(BUILD_DIR)/sw/$$d.$$name.out; \
	    if $$sim --quiet $$elf > $$out; then \
	      if [ -f tests/$$d/expected.txt ] && ! cmp -s tests/$$d/expected.txt $$out; then \
	        echo "FAIL $$d on $$name (output differs from expected.txt)"; status=1; \
	      else echo "PASS $$d on $$name"; fi; \
	    else echo "FAIL $$d on $$name (nonzero exit)"; status=1; fi; \
	  done; \
	done; exit $$status

clean:
	rm -rf ../build/sw
